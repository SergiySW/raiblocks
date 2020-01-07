#include <nano/node/bootstrap/bootstrap.hpp>
#include <nano/node/bootstrap/bootstrap_lazy.hpp>
#include <nano/node/common.hpp>
#include <nano/node/node.hpp>
#include <nano/node/transport/tcp.hpp>

#include <boost/format.hpp>

#include <algorithm>

constexpr double nano::bootstrap_limits::bootstrap_connection_scale_target_blocks_lazy;
constexpr std::chrono::seconds nano::bootstrap_limits::lazy_flush_delay_sec;
constexpr unsigned nano::bootstrap_limits::lazy_destinations_request_limit;
constexpr uint64_t nano::bootstrap_limits::lazy_batch_pull_count_resize_blocks_limit;
constexpr double nano::bootstrap_limits::lazy_batch_pull_count_resize_ratio;
constexpr size_t nano::bootstrap_limits::lazy_blocks_restart_limit;

nano::bootstrap_attempt_lazy::bootstrap_attempt_lazy (std::shared_ptr<nano::node> node_a, nano::bootstrap_mode mode_a) :
nano::bootstrap_attempt (node_a, mode_a)
{
	assert (mode == nano::bootstrap_mode::lazy);
	node->logger.always_log ("Starting lazy bootstrap attempt");
	node->bootstrap_initiator.notify_listeners (true);
}

nano::bootstrap_attempt_lazy::~bootstrap_attempt_lazy ()
{
	node->logger.always_log ("Exiting lazy bootstrap attempt");
	node->bootstrap_initiator.notify_listeners (false);
}

void nano::bootstrap_attempt_lazy::request_pull_lazy (nano::unique_lock<std::mutex> & lock_a)
{
	auto connection_l (connection (lock_a));
	if (connection_l)
	{
		auto pull (pulls.front ());
		pulls.pop_front ();
		// Check if pull is obsolete (head was processed)
		while (!pulls.empty () && !pull.head.is_zero () && lazy_processed_or_exists (pull.head))
		{
			pull = pulls.front ();
			pulls.pop_front ();
		}
		++pulling;
		// The bulk_pull_client destructor attempt to requeue_pull which can cause a deadlock if this is the last reference
		// Dispatch request in an external thread in case it needs to be destroyed
		node->background ([connection_l, pull]() {
			auto client (std::make_shared<nano::bulk_pull_client> (connection_l, pull));
			client->request ();
		});
	}
}

void nano::bootstrap_attempt_lazy::requeue_pull_lazy (nano::pull_info const & pull_a, bool network_error)
{
	auto pull (pull_a);
	if (!network_error)
	{
		++pull.attempts;
	}
	++requeued_pulls;
	if (mode != nano::bootstrap_mode::lazy && pull.attempts < pull.retry_limit + (pull.processed / 10000))
	{
		nano::lock_guard<std::mutex> lock (mutex);
		pulls.push_front (pull);
		condition.notify_all ();
	}
	else if (mode == nano::bootstrap_mode::lazy && (pull.retry_limit == std::numeric_limits<unsigned>::max () || pull.attempts <= pull.retry_limit + (pull.processed / node->network_params.bootstrap.lazy_max_pull_blocks)))
	{
		assert (pull.account_or_head == pull.head);
		if (!lazy_processed_or_exists (pull.account_or_head))
		{
			// Retry for lazy pulls
			nano::lock_guard<std::mutex> lock (mutex);
			pulls.push_back (pull);
			condition.notify_all ();
		}
	}
	else
	{
		if (node->config.logging.bulk_pull_logging ())
		{
			node->logger.try_log (boost::str (boost::format ("Failed to pull account %1% down to %2% after %3% attempts and %4% blocks processed") % pull.account_or_head.to_account () % pull.end.to_string () % pull.attempts % pull.processed));
		}
		node->stats.inc (nano::stat::type::bootstrap, nano::stat::detail::bulk_pull_failed_account, nano::stat::dir::in);

		node->bootstrap_initiator.cache.add (pull);
		if (mode == nano::bootstrap_mode::lazy && pull.processed > 0)
		{
			assert (pull.account_or_head == pull.head);
			nano::lock_guard<std::mutex> lazy_lock (lazy_mutex);
			lazy_add (pull.account_or_head, pull.retry_limit);
		}
	}
}

void nano::bootstrap_attempt_lazy::lazy_start (nano::hash_or_account const & hash_or_account_a, bool confirmed)
{
	nano::lock_guard<std::mutex> lazy_lock (lazy_mutex);
	// Add start blocks, limit 1024 (4k with disabled legacy bootstrap)
	size_t max_keys (node->flags.disable_legacy_bootstrap ? 4 * 1024 : 1024);
	if (lazy_keys.size () < max_keys && lazy_keys.find (hash_or_account_a) == lazy_keys.end () && !lazy_blocks_processed (hash_or_account_a))
	{
		lazy_keys.insert (hash_or_account_a);
		lazy_pulls.emplace_back (hash_or_account_a, confirmed ? std::numeric_limits<unsigned>::max () : node->network_params.bootstrap.lazy_retry_limit);
	}
}

void nano::bootstrap_attempt_lazy::lazy_add (nano::hash_or_account const & hash_or_account_a, unsigned retry_limit)
{
	// Add only unknown blocks
	assert (!lazy_mutex.try_lock ());
	if (!lazy_blocks_processed (hash_or_account_a))
	{
		lazy_pulls.emplace_back (hash_or_account_a, retry_limit);
	}
}

void nano::bootstrap_attempt_lazy::lazy_requeue (nano::block_hash const & hash_a, nano::block_hash const & previous_a, bool confirmed_a)
{
	nano::unique_lock<std::mutex> lazy_lock (lazy_mutex);
	// Add only known blocks
	if (lazy_blocks_processed (hash_a))
	{
		lazy_blocks_erase (hash_a);
		lazy_lock.unlock ();
		requeue_pull_lazy (nano::pull_info (hash_a, hash_a, previous_a, static_cast<nano::pull_info::count_t> (1), confirmed_a ? std::numeric_limits<unsigned>::max () : node->network_params.bootstrap.lazy_destinations_retry_limit));
	}
}

void nano::bootstrap_attempt_lazy::lazy_pull_flush ()
{
	assert (!mutex.try_lock ());
	static size_t const max_pulls (nano::bootstrap_limits::bootstrap_connection_scale_target_blocks_lazy * 3);
	if (pulls.size () < max_pulls)
	{
		last_lazy_flush = std::chrono::steady_clock::now ();
		nano::lock_guard<std::mutex> lazy_lock (lazy_mutex);
		assert (node->network_params.bootstrap.lazy_max_pull_blocks <= std::numeric_limits<nano::pull_info::count_t>::max ());
		nano::pull_info::count_t batch_count (node->network_params.bootstrap.lazy_max_pull_blocks);
		if (total_blocks > nano::bootstrap_limits::lazy_batch_pull_count_resize_blocks_limit && !lazy_blocks.empty ())
		{
			double lazy_blocks_ratio (total_blocks / lazy_blocks.size ());
			if (lazy_blocks_ratio > nano::bootstrap_limits::lazy_batch_pull_count_resize_ratio)
			{
				// Increasing blocks ratio weight as more important (^3). Small batch count should lower blocks ratio below target
				double lazy_blocks_factor (std::pow (lazy_blocks_ratio / nano::bootstrap_limits::lazy_batch_pull_count_resize_ratio, 3.0));
				// Decreasing total block count weight as less important (sqrt)
				double total_blocks_factor (std::sqrt (total_blocks / nano::bootstrap_limits::lazy_batch_pull_count_resize_blocks_limit));
				uint32_t batch_count_min (node->network_params.bootstrap.lazy_max_pull_blocks / (lazy_blocks_factor * total_blocks_factor));
				batch_count = std::max (node->network_params.bootstrap.lazy_min_pull_blocks, batch_count_min);
			}
		}
		size_t count (0);
		auto transaction (node->store.tx_begin_read ());
		while (!lazy_pulls.empty () && count < max_pulls)
		{
			auto const & pull_start (lazy_pulls.front ());
			// Recheck if block was already processed
			if (!lazy_blocks_processed (pull_start.first) && !node->store.block_exists (transaction, pull_start.first))
			{
				pulls.emplace_back (pull_start.first, pull_start.first, nano::block_hash (0), batch_count, pull_start.second);
				++count;
			}
			lazy_pulls.pop_front ();
		}
	}
}

bool nano::bootstrap_attempt_lazy::lazy_finished ()
{
	if (stopped)
	{
		return true;
	}
	bool result (true);
	auto transaction (node->store.tx_begin_read ());
	nano::lock_guard<std::mutex> lazy_lock (lazy_mutex);
	for (auto it (lazy_keys.begin ()), end (lazy_keys.end ()); it != end && !stopped;)
	{
		if (node->store.block_exists (transaction, *it))
		{
			it = lazy_keys.erase (it);
		}
		else
		{
			result = false;
			break;
			// No need to increment `it` as we break above.
		}
	}
	// Finish lazy bootstrap without lazy pulls (in combination with still_pulling ())
	if (!result && lazy_pulls.empty () && lazy_state_backlog.empty ())
	{
		result = true;
	}
	// Don't close lazy bootstrap until all destinations are processed
	if (result && !lazy_destinations.empty ())
	{
		result = false;
	}
	return result;
}

bool nano::bootstrap_attempt_lazy::lazy_has_expired () const
{
	bool result (false);
	// Max 30 minutes run with enabled legacy bootstrap
	static std::chrono::minutes const max_lazy_time (node->flags.disable_legacy_bootstrap ? 7 * 24 * 60 : 30);
	if (std::chrono::steady_clock::now () - lazy_start_time >= max_lazy_time)
	{
		result = true;
	}
	else if (!node->flags.disable_legacy_bootstrap && lazy_blocks_count > nano::bootstrap_limits::lazy_blocks_restart_limit)
	{
		result = true;
	}
	return result;
}

void nano::bootstrap_attempt_lazy::lazy_run ()
{
	assert (!node->flags.disable_lazy_bootstrap);
	start_populate_connections ();
	lazy_start_time = std::chrono::steady_clock::now ();
	nano::unique_lock<std::mutex> lock (mutex);
	while ((still_pulling () || !lazy_finished ()) && !lazy_has_expired ())
	{
		unsigned iterations (0);
		while (still_pulling () && !lazy_has_expired ())
		{
			if (!pulls.empty ())
			{
				request_pull_lazy (lock);
			}
			else
			{
				lazy_pull_flush ();
				if (pulls.empty ())
				{
					condition.wait_for (lock, std::chrono::seconds (1));
				}
			}
			++iterations;
			// Flushing lazy pulls
			if (iterations % 100 == 0 || last_lazy_flush + nano::bootstrap_limits::lazy_flush_delay_sec < std::chrono::steady_clock::now ())
			{
				lazy_pull_flush ();
			}
			// Start backlog cleanup
			if (iterations % 200 == 0)
			{
				lazy_backlog_cleanup ();
			}
			// Destinations check
			if (pulls.empty () && lazy_destinations_flushed)
			{
				lazy_destinations_flush ();
				lazy_pull_flush ();
			}
		}
		// Flushing lazy pulls
		lazy_pull_flush ();
		// Check if some blocks required for backlog were processed. Start destinations check
		if (pulls.empty ())
		{
			lazy_backlog_cleanup ();
			lazy_destinations_flush ();
			lazy_pull_flush ();
		}
	}
	if (!stopped)
	{
		node->logger.try_log ("Completed lazy pulls");
	}
	stopped = true;
	condition.notify_all ();
	idle.clear ();
}

bool nano::bootstrap_attempt_lazy::process_block (std::shared_ptr<nano::block> block_a, nano::account const & known_account_a, uint64_t pull_blocks, nano::bulk_pull::count_t max_blocks, bool block_expected, unsigned retry_limit)
{
	bool stop_pull (false);
	if (block_expected)
	{
		stop_pull = process_block_lazy (block_a, known_account_a, pull_blocks, max_blocks, retry_limit);
	}
	else
	{
		// Drop connection with unexpected block for lazy bootstrap
		stop_pull = true;
	}
	return stop_pull;
}

bool nano::bootstrap_attempt_lazy::process_block_lazy (std::shared_ptr<nano::block> block_a, nano::account const & known_account_a, uint64_t pull_blocks, nano::bulk_pull::count_t max_blocks, unsigned retry_limit)
{
	bool stop_pull (false);
	auto hash (block_a->hash ());
	nano::unique_lock<std::mutex> lazy_lock (lazy_mutex);
	// Processing new blocks
	if (!lazy_blocks_processed (hash))
	{
		// Search for new dependencies
		if (!block_a->source ().is_zero () && !node->ledger.block_exists (block_a->source ()) && block_a->source () != node->network_params.ledger.genesis_account)
		{
			lazy_add (block_a->source (), retry_limit);
		}
		else if (block_a->type () == nano::block_type::state)
		{
			lazy_block_state (block_a, retry_limit);
		}
		else if (block_a->type () == nano::block_type::send)
		{
			std::shared_ptr<nano::send_block> block_l (std::static_pointer_cast<nano::send_block> (block_a));
			if (block_l != nullptr && !block_l->hashables.destination.is_zero ())
			{
				lazy_destinations_increment (block_l->hashables.destination);
			}
		}
		lazy_blocks_insert (hash);
		// Adding lazy balances for first processed block in pull
		if (pull_blocks == 0 && (block_a->type () == nano::block_type::state || block_a->type () == nano::block_type::send))
		{
			lazy_balances.emplace (hash, block_a->balance ().number ());
		}
		// Clearing lazy balances for previous block
		if (!block_a->previous ().is_zero () && lazy_balances.find (block_a->previous ()) != lazy_balances.end ())
		{
			lazy_balances.erase (block_a->previous ());
		}
		lazy_block_state_backlog_check (block_a, hash);
		lazy_lock.unlock ();
		nano::unchecked_info info (block_a, known_account_a, 0, nano::signature_verification::unknown, retry_limit == std::numeric_limits<unsigned>::max ());
		node->block_processor.add (info);
	}
	// Force drop lazy bootstrap connection for long bulk_pull
	if (pull_blocks > max_blocks)
	{
		stop_pull = true;
	}
	return stop_pull;
}

void nano::bootstrap_attempt_lazy::lazy_block_state (std::shared_ptr<nano::block> block_a, unsigned retry_limit)
{
	std::shared_ptr<nano::state_block> block_l (std::static_pointer_cast<nano::state_block> (block_a));
	if (block_l != nullptr)
	{
		auto transaction (node->store.tx_begin_read ());
		nano::uint128_t balance (block_l->hashables.balance.number ());
		auto const & link (block_l->hashables.link);
		// If link is not epoch link or 0. And if block from link is unknown
		if (!link.is_zero () && !node->ledger.is_epoch_link (link) && !lazy_blocks_processed (link) && !node->store.block_exists (transaction, link))
		{
			auto const & previous (block_l->hashables.previous);
			// If state block previous is 0 then source block required
			if (previous.is_zero ())
			{
				lazy_add (link, retry_limit);
			}
			// In other cases previous block balance required to find out subtype of state block
			else if (node->store.block_exists (transaction, previous))
			{
				if (node->ledger.balance (transaction, previous) <= balance)
				{
					lazy_add (link, retry_limit);
				}
				else
				{
					lazy_destinations_increment (link);
				}
			}
			// Search balance of already processed previous blocks
			else if (lazy_blocks_processed (previous))
			{
				auto previous_balance (lazy_balances.find (previous));
				if (previous_balance != lazy_balances.end ())
				{
					if (previous_balance->second <= balance)
					{
						lazy_add (link, retry_limit);
					}
					else
					{
						lazy_destinations_increment (link);
					}
					lazy_balances.erase (previous_balance);
				}
			}
			// Insert in backlog state blocks if previous wasn't already processed
			else
			{
				lazy_state_backlog.emplace (previous, nano::lazy_state_backlog_item{ link, balance, retry_limit });
			}
		}
	}
}

void nano::bootstrap_attempt_lazy::lazy_block_state_backlog_check (std::shared_ptr<nano::block> block_a, nano::block_hash const & hash_a)
{
	// Search unknown state blocks balances
	auto find_state (lazy_state_backlog.find (hash_a));
	if (find_state != lazy_state_backlog.end ())
	{
		auto next_block (find_state->second);
		// Retrieve balance for previous state & send blocks
		if (block_a->type () == nano::block_type::state || block_a->type () == nano::block_type::send)
		{
			if (block_a->balance ().number () <= next_block.balance) // balance
			{
				lazy_add (next_block.link, next_block.retry_limit); // link
			}
			else
			{
				lazy_destinations_increment (next_block.link);
			}
		}
		// Assumption for other legacy block types
		else if (lazy_undefined_links.find (next_block.link) == lazy_undefined_links.end ())
		{
			lazy_add (next_block.link, node->network_params.bootstrap.lazy_retry_limit); // Head is not confirmed. It can be account or hash or non-existing
			lazy_undefined_links.insert (next_block.link);
		}
		lazy_state_backlog.erase (find_state);
	}
}

void nano::bootstrap_attempt_lazy::lazy_backlog_cleanup ()
{
	auto transaction (node->store.tx_begin_read ());
	nano::lock_guard<std::mutex> lazy_lock (lazy_mutex);
	for (auto it (lazy_state_backlog.begin ()), end (lazy_state_backlog.end ()); it != end && !stopped;)
	{
		if (node->store.block_exists (transaction, it->first))
		{
			auto next_block (it->second);
			if (node->ledger.balance (transaction, it->first) <= next_block.balance) // balance
			{
				lazy_add (next_block.link, next_block.retry_limit); // link
			}
			else
			{
				lazy_destinations_increment (next_block.link);
			}
			it = lazy_state_backlog.erase (it);
		}
		else
		{
			lazy_add (it->first, it->second.retry_limit);
			++it;
		}
	}
}

void nano::bootstrap_attempt_lazy::lazy_destinations_increment (nano::account const & destination_a)
{
	// Enabled only if legacy bootstrap is not available. Legacy bootstrap is a more effective way to receive all existing destinations
	if (node->flags.disable_legacy_bootstrap)
	{
		// Update accounts counter for send blocks
		auto existing (lazy_destinations.get<account_tag> ().find (destination_a));
		if (existing != lazy_destinations.get<account_tag> ().end ())
		{
			lazy_destinations.get<account_tag> ().modify (existing, [](nano::lazy_destinations_item & item_a) {
				++item_a.count;
			});
		}
		else
		{
			lazy_destinations.emplace (nano::lazy_destinations_item{ destination_a, 1 });
		}
	}
}

void nano::bootstrap_attempt_lazy::lazy_destinations_flush ()
{
	lazy_destinations_flushed = true;
	size_t count (0);
	nano::lock_guard<std::mutex> lazy_lock (lazy_mutex);
	for (auto it (lazy_destinations.get<count_tag> ().begin ()), end (lazy_destinations.get<count_tag> ().end ()); it != end && count < nano::bootstrap_limits::lazy_destinations_request_limit && !stopped;)
	{
		lazy_add (it->account, node->network_params.bootstrap.lazy_destinations_retry_limit);
		it = lazy_destinations.get<count_tag> ().erase (it);
		++count;
	}
}

void nano::bootstrap_attempt_lazy::lazy_blocks_insert (nano::block_hash const & hash_a)
{
	assert (!lazy_mutex.try_lock ());
	auto inserted (lazy_blocks.insert (std::hash<::nano::block_hash> () (hash_a)));
	if (inserted.second)
	{
		++lazy_blocks_count;
	}
}

void nano::bootstrap_attempt_lazy::lazy_blocks_erase (nano::block_hash const & hash_a)
{
	assert (!lazy_mutex.try_lock ());
	auto erased (lazy_blocks.erase (std::hash<::nano::block_hash> () (hash_a)));
	if (erased)
	{
		--lazy_blocks_count;
	}
}

bool nano::bootstrap_attempt_lazy::lazy_blocks_processed (nano::block_hash const & hash_a)
{
	return lazy_blocks.find (std::hash<::nano::block_hash> () (hash_a)) != lazy_blocks.end ();
}

bool nano::bootstrap_attempt_lazy::lazy_processed_or_exists (nano::block_hash const & hash_a)
{
	bool result (false);
	nano::unique_lock<std::mutex> lazy_lock (lazy_mutex);
	if (lazy_blocks_processed (hash_a))
	{
		result = true;
	}
	else
	{
		lazy_lock.unlock ();
		if (node->ledger.block_exists (hash_a))
		{
			result = true;
		}
	}
	return result;
}

void nano::bootstrap_attempt_lazy::requeue_pending (nano::account const &)
{
	assert (false);
}

size_t nano::bootstrap_attempt_lazy::wallet_size ()
{
	assert (false);
	return 0;
}

nano::bootstrap_attempt_wallet::bootstrap_attempt_wallet (std::shared_ptr<nano::node> node_a, nano::bootstrap_mode mode_a) :
nano::bootstrap_attempt (node_a, mode_a)
{
	assert (mode == nano::bootstrap_mode::wallet_lazy);
	node->logger.always_log ("Starting wallet bootstrap attempt");
	node->bootstrap_initiator.notify_listeners (true);
}

nano::bootstrap_attempt_wallet::~bootstrap_attempt_wallet ()
{
	node->logger.always_log ("Exiting wallet bootstrap attempt");
	node->bootstrap_initiator.notify_listeners (false);
}

void nano::bootstrap_attempt_wallet::request_pending (nano::unique_lock<std::mutex> & lock_a)
{
	auto connection_l (connection (lock_a));
	if (connection_l)
	{
		auto account (wallet_accounts.front ());
		wallet_accounts.pop_front ();
		++pulling;
		// The bulk_pull_account_client destructor attempt to requeue_pull which can cause a deadlock if this is the last reference
		// Dispatch request in an external thread in case it needs to be destroyed
		node->background ([connection_l, account]() {
			auto client (std::make_shared<nano::bulk_pull_account_client> (connection_l, account));
			client->request ();
		});
	}
}

void nano::bootstrap_attempt_wallet::requeue_pending (nano::account const & account_a)
{
	auto account (account_a);
	{
		nano::lock_guard<std::mutex> lock (mutex);
		wallet_accounts.push_front (account);
		condition.notify_all ();
	}
}

void nano::bootstrap_attempt_wallet::wallet_start (std::deque<nano::account> & accounts_a)
{
	nano::lock_guard<std::mutex> lock (mutex);
	wallet_accounts.swap (accounts_a);
	condition.notify_all ();
}

bool nano::bootstrap_attempt_wallet::wallet_finished ()
{
	assert (!mutex.try_lock ());
	auto running (!stopped);
	auto more_accounts (!wallet_accounts.empty ());
	auto still_pulling (pulling > 0);
	return running && (more_accounts || still_pulling);
}

void nano::bootstrap_attempt_wallet::wallet_run ()
{
	assert (!node->flags.disable_wallet_bootstrap);
	start_populate_connections ();
	auto start_time (std::chrono::steady_clock::now ());
	auto max_time (std::chrono::minutes (10));
	nano::unique_lock<std::mutex> lock (mutex);
	while (wallet_finished () && std::chrono::steady_clock::now () - start_time < max_time)
	{
		if (!wallet_accounts.empty ())
		{
			request_pending (lock);
		}
		else
		{
			condition.wait_for (lock, std::chrono::seconds (1));
		}
	}
	if (!stopped)
	{
		node->logger.try_log ("Completed wallet lazy pulls");
	}
	stopped = true;
	condition.notify_all ();
}

bool nano::bootstrap_attempt_wallet::process_block (std::shared_ptr<nano::block> block_a, nano::account const & known_account_a, uint64_t pull_blocks, nano::bulk_pull::count_t max_blocks, bool block_expected, unsigned retry_limit)
{
	assert (false);
	return false;
}

size_t nano::bootstrap_attempt_wallet::wallet_size ()
{
	nano::lock_guard<std::mutex> lock (mutex);
	return wallet_accounts.size ();
}

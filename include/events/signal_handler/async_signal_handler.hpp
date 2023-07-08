#pragma once

#include <algorithm>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

#include <plf_colony.h>


namespace events {

namespace detail {
/**
 * @brief Publish a set of operations as a parallel_group, discarding the return order and supporting operations that
 *        take a nullary completion handler (i.e. invoke a completion with no parameters).
 *
 * @details Operations with nullary completions will still need to return something (e.g. std::monostate). This
 *          function will discard those results and invoke a completion with no parameters.
 *
 * @tparam ResultsT...  The completion parameters of the operations
 */
template<typename... ResultsT, typename Operations, typename CompletionToken>
auto parallel_publish(auto default_executor, Operations&& operations, CompletionToken&& completion_token) {
	using completion_type = std::conditional_t<
		std::conjunction_v<std::is_same<void, ResultsT>...>,
		void(),
		void(std::vector<ResultsT>...)
	>;

	return boost::asio::async_initiate<CompletionToken, completion_type>(
		[&default_executor, ops = std::forward<Operations>(operations)](auto completion_handler) mutable {
			if (ops.empty()) {
			    auto completion_ex = boost::asio::get_associated_executor(completion_handler, default_executor);

				boost::asio::post(
					completion_ex,
					[handler = std::move(completion_handler)]() mutable {
						if constexpr ((std::same_as<void, ResultsT> && ...)) {
							std::move(handler)();
						}
						else {
					        std::move(handler)(std::vector<ResultsT>{}...);
						}
					}
				);
			}
			else {
				boost::asio::experimental::make_parallel_group(
					std::move(ops)
				).async_wait(
					boost::asio::experimental::wait_for_all{},
					//NOLINTNEXTLINE(performance-unnecessary-value-param)
					[handler = std::move(completion_handler)](std::vector<size_t> /*completion_order*/, [[maybe_unused]] auto... results) mutable {
						if constexpr ((std::same_as<void, ResultsT> && ...)) {
							std::move(handler)();
						}
						else {
							std::move(handler)(std::move(results)...);
						}
					}
				);
			}
		},
		completion_token
	);
}
} //namespace detail


/// Defines the how a callback is handled when a signal is fired while the callback is still executing
struct callback_policy {
	struct drop {};  ///< The callback will drop the signal if it hasn't finished processing the last signal
	struct concurrent {};  ///< The callback will be launched regardless of whether it has finished processing the last signal
};


template<typename...>
class async_signal_handler;


/**
 * @brief A signal handler that invokes callbacks asynchronously. Callbacks that don't finish before a new signal is
 *        published will drop the signal.
 */
template<typename ReturnT, typename... ArgsT>
class [[nodiscard]] async_signal_handler<ReturnT(ArgsT...), callback_policy::drop> : public std::enable_shared_from_this<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop>> {
	using container_type = plf::colony<std::function<ReturnT(ArgsT...)>>;

	struct passkey {
		explicit passkey() = default;
	};

public:
	using completion_type = std::conditional_t<std::is_same_v<void, ReturnT>, void(), void(std::vector<ReturnT>)>;

	using function_type = ReturnT(ArgsT...);

	async_signal_handler([[maybe_unused]] passkey key, boost::asio::any_io_executor exec) : executor(std::move(exec)) {
	}

	template<typename ExecutionContext>
	async_signal_handler([[maybe_unused]] passkey key, ExecutionContext& exec) : executor(exec.get_executor()) {
	}

	async_signal_handler(async_signal_handler const&) = delete;
	async_signal_handler(async_signal_handler&&) noexcept = default;

	~async_signal_handler() = default;

	auto operator=(async_signal_handler const&) -> async_signal_handler& = delete;
	auto operator=(async_signal_handler&&) noexcept -> async_signal_handler& = default;

	/// Create an instance of an async_signal_handler
	template<typename... ConstructorArgsT>
	[[nodiscard]]
	static auto create(ConstructorArgsT&&... args) -> std::shared_ptr<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop>> {
		return std::make_shared<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop>>(passkey{}, std::forward<ConstructorArgsT>(args)...);
	}

	/// Get the executor associated with this object
	[[nodiscard]]
	auto get_executor() -> boost::asio::any_io_executor {
		return executor;
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired
	 *
	 * @tparam FunctionT
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<typename FunctionT>
	auto connect(FunctionT&& func) -> connection {
		auto lock = std::unique_lock{callback_mut};
		auto const it = callbacks.insert(std::forward<FunctionT>(func));
		lock.unlock();

		{
			auto working_lock = std::scoped_lock{working_callback_mut};
			working_callbacks[&(*it)] = false;
		}

		return connection{[weak_self = this->weak_from_this(), ptr = &(*it)] {
			if (auto self = weak_self.lock()) {
				self->disconnect(ptr);
			}
		}};
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT> {
		auto pending_callbacks = prune_callbacks_and_get_pending();

		for (auto ptr : pending_callbacks) {
			(*ptr)(args...);
			release_callback(ptr);
		}
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires (!std::same_as<void, ReturnT>) {
		auto pending_callbacks = prune_callbacks_and_get_pending();

		auto results = std::vector<ReturnT>{};
		results.reserve(pending_callbacks.size());

		for (auto ptr : pending_callbacks) {
			results.emplace_back((*ptr)(args...));
			release_callback(ptr);
		}

		return results;
	}

	/**
	 * @brief Fire the signal asynchronously
	 *
	 * @param args The signal arguments
	 */
	auto async_publish(ArgsT... args) -> void {
		// Store the arguments in a tuple, which will be referenced by each async operation.
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		// Delete callbacks marked for removal, and get a list of those that can be executed. No lock on callback_mut
		// needs to be acquired for the rest of this function. This is because the callbacks will be marked as running
		// and thus can not be deleted until released via release_callback().
		auto const pending_callbacks = prune_callbacks_and_get_pending();

		for (auto ptr : pending_callbacks) {
			boost::asio::post(
				executor,
				[self = this->shared_from_this(), ptr, args_tuple]() mutable {
					(void)std::apply(*ptr, *args_tuple);
				}
			);
		}
	}

	/**
	 * @brief Fire the signal asynchronously and invoke a completion token when finished
	 *
	 * @param args The signal arguments
	 * @param completion A completion token that will be called when the operation completes
	 */
	template<boost::asio::completion_token_for<completion_type> CompletionToken>
	auto async_publish(ArgsT... args, CompletionToken&& completion) {
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		auto const pending_callbacks = prune_callbacks_and_get_pending();

		// This will post a function which will invoke the callback then re-add the callback to the list of pending
		// callbacks once it has completed. The actual operation is deferred to be later executed as part of a
		// parallel_group.
		auto post_op = [this, &args_tuple](typename container_type::pointer ptr) {
			return boost::asio::post(
				executor,
				boost::asio::deferred([this, ptr, &args = *args_tuple]() mutable {
					if constexpr (std::same_as<void, ReturnT>) {
						std::apply(*ptr, args);
						release_callback(ptr);
						return boost::asio::deferred_t::values(std::monostate{}); //needs to return something, void return won't work
					}
					else {
						auto result = std::apply(*ptr, args);
						release_callback(ptr);
						return boost::asio::deferred_t::values(std::move(result));
					}
				})
			);
		};

		// Create the deferred callback invocation for each callback
		using post_op_type = decltype(post_op({}));
		auto operations = std::vector<post_op_type>{};
		operations.reserve(pending_callbacks.size());

		for (auto ptr : pending_callbacks) {
			operations.emplace_back(post_op(ptr));
		}

		// Keep this signal handler and the arguments alive until the completion token has been called (when all
		// callbacks have completed).
		auto consigned = boost::asio::consign(std::forward<CompletionToken>(completion), this->shared_from_this(), std::move(args_tuple));
		
		// Initiate the callbacks as a parallel_group, with a completion that takes either nothing if ReturnT is void,
		// or a vector of the callback results otherwise.
		return detail::parallel_publish<ReturnT>(executor, std::move(operations), std::move(consigned));
	}

private:
	auto disconnect(typename container_type::const_pointer pointer) -> void {
		auto lock = std::scoped_lock{to_remove_mut};
		to_remove.push_back(pointer);
	}

	auto release_callback(typename container_type::const_pointer pointer) -> void {
		auto lock = std::shared_lock{working_callback_mut};
		working_callbacks[pointer] = false;
	}

	// Remove invalidated callbacks and get a list of the pending ones
	[[nodiscard]]
	auto prune_callbacks_and_get_pending() -> std::vector<typename container_type::pointer> {
		// This will be populated with the callbacks that need to be executed
		auto pending_callbacks = std::vector<typename container_type::pointer>{};
		pending_callbacks.reserve(callbacks.size() - working_callbacks.size()); //reserve an estimate of the final size

		auto working_lock = std::scoped_lock{callback_mut, working_callback_mut};

		// Remove invalidated callbacks that aren't currently executing
		remove_invalidated_callbacks();

		// Gather all pending (not running) callbacks. We don't need to filter out handles in the to_remove set since
		// the function call above will only leave invalidated callbacks that are currently executing, and they can't
		// be released during this block since an exclusive mutex is held the entire time. Thus, they won't be included
		// in this update and will be removed with the next update.
		for (auto& callback : callbacks) {
			if (working_callbacks[&callback]) {
				//SPDLOG_LOG_TRACE("Callback {:#X} dropped a signal", static_cast<uintptr_t>(&(*it)));
			}
			else {
				pending_callbacks.push_back(&callback);
				working_callbacks[&callback] = true;
			}
		}

		return pending_callbacks;
	}

	// Requires an exclusive lock on callback_mut and working_callback_mut
	auto remove_invalidated_callbacks() -> void {
		auto lock = std::scoped_lock{to_remove_mut};

		// Move all handles whose callbacks aren't currently executing to the end of the vector
		auto const remove_begin = std::partition(
			to_remove.begin(),
			to_remove.end(),
			[&](auto pointer) { return working_callbacks[pointer]; }
		);

		// Erase all callbacks that aren't in progress
		for (auto it = remove_begin; it != to_remove.end(); ++it) {
			working_callbacks.erase(*it);
			callbacks.erase(callbacks.get_iterator(*it));
		}

		// Erase all the now-invalid iterators
		auto const count = static_cast<size_t>(std::distance(remove_begin, to_remove.end()));
		to_remove.resize(to_remove.size() - count);
	}


	boost::asio::any_io_executor executor;

	container_type callbacks;
	std::mutex callback_mut;

	// Tracks the execution state of each callback
	std::unordered_map<typename container_type::const_pointer, bool> working_callbacks;
	std::shared_mutex working_callback_mut;

	// Tracks which callbacks have been disconnected, so they can be removed when they are no longer executing. This
	// part could be removed if callbacks were stored as std::shared_ptr<std::function<...>>, at the cost of extra
	// indirection, but that doesn't simplify the design too much since the callback execution state has to be tracked
	// as well. Manually managing the callback lifetime is only a little extra work.
	std::vector<typename container_type::const_pointer> to_remove;
	std::mutex to_remove_mut;
};



/**
 * @brief A signal handler that invokes callbacks asynchronously. Callbacks that don't finish before a new signal is
 *        published will still be invoked.
 */
template<typename ReturnT, typename... ArgsT>
class [[nodiscard]] async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent> : public std::enable_shared_from_this<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent>> {
	using container_type = plf::colony<std::shared_ptr<std::function<ReturnT(ArgsT...)>>>;
	using completion_type = std::conditional_t<std::is_same_v<void, ReturnT>, void(), void(std::vector<ReturnT>)>;

	struct passkey {
		explicit passkey() = default;
	};

public:
	using function_type = ReturnT(ArgsT...);

	async_signal_handler([[maybe_unused]] passkey key, boost::asio::any_io_executor exec) : executor(std::move(exec)) {
	}

	template<typename ExecutionContext>
	async_signal_handler([[maybe_unused]] passkey key, ExecutionContext& exec) : executor(exec.get_executor()) {
	}

	/// Create an instance of an async_signal_handler
	template<typename... ConstructorArgsT>
	[[nodiscard]]
	static auto create(ConstructorArgsT&&... args) -> std::shared_ptr<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent>> {
		return std::make_shared<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent>>(passkey{}, std::forward<ConstructorArgsT>(args)...);
	}

	/// Get the executor associated with this object
	[[nodiscard]]
	auto get_executor() -> boost::asio::any_io_executor {
		return executor;
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired
	 *
	 * @tparam FunctionT
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<typename FunctionT>
	auto connect(FunctionT&& func) -> connection {
		auto lock = std::unique_lock{callback_mut};
		auto const it = callbacks.insert(std::make_shared<std::function<function_type>>(std::forward<FunctionT>(func)));
		lock.unlock();

		return connection{[weak_self = this->weak_from_this(), ptr = &(*it)] {
			if (auto self = weak_self.lock()) {
				self->disconnect(ptr);
			}
		}};
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT> {
		auto lock = std::shared_lock{callback_mut};

		for (auto& callback_ptr : callbacks) {
			(*callback_ptr)(args...);
		}
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires (!std::same_as<void, ReturnT>) {
		auto lock = std::shared_lock{callback_mut};

		auto results = std::vector<ReturnT>{};
		results.reserve(callbacks.size());

		for (auto& callback_ptr : callbacks) {
			results.emplace_back((*callback_ptr)(args...));
		}

		return results;
	}

	/**
	 * @brief Fire the signal asynchronously
	 *
	 * @param args The signal arguments
	 */
	auto async_publish(ArgsT... args) -> void {
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		auto lock = std::shared_lock{callback_mut};

		for (auto& callback_ptr : callbacks) {
			boost::asio::post(
				executor,
				[self = this->shared_from_this(), callback_ptr, args_tuple]() mutable {
					(void)std::apply(*callback_ptr, args_tuple);
				}
			);
		}
	}

	/**
	 * @brief Fire the signal asynchronously and invoke a completion token when finished
	 *
	 * @param args The signal arguments
	 * @param completion A completion token that will be called when the operation completes
	 */
	template<typename CompletionToken>
	auto async_publish(ArgsT... args, CompletionToken&& completion) {
		// Store the arguments in a tuple, which will be referenced by each async operation.
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		// This will post a function which will invoke the callback then re-add itself to the list of pending callbacks
		// once it has completed. The actual operation is deferred to be later executed as part of a parallel_group.
		auto post_op = [this, &args_tuple](typename container_type::reference callback_ptr) {
			return boost::asio::post(
				executor,
				boost::asio::deferred([callback_ptr, &args = *args_tuple]() mutable {
					if constexpr (std::same_as<void, ReturnT>) {
						std::apply(*callback_ptr, args);
						return boost::asio::deferred_t::values(std::monostate{}); //needs to return something, void return won't work
					}
					else {
						auto result = std::apply(*callback_ptr, args);
						return boost::asio::deferred_t::values(std::move(result));
					}
				})
			);
		};

		using post_op_type = decltype(post_op(std::declval<typename container_type::reference>()));
		auto operations = std::vector<post_op_type>{};
		operations.reserve(callbacks.size());

		auto lock = std::shared_lock{callback_mut};

		// Create a deferred callback invocation for each callback
		for (auto& ptr : callbacks) {
			operations.emplace_back(post_op(ptr));
		}

		// Keep this signal handler and the arguments alive until the completion token has been called (when all
		// callbacks have completed).
		auto consigned = boost::asio::consign(std::forward<CompletionToken>(completion), this->shared_from_this(), std::move(args_tuple));

		// Initiate the callbacks as a parallel_group, with a completion that takes either nothing if ReturnT is void,
		// or a vector of the callback results otherwise.
		return detail::parallel_publish<ReturnT>(executor, std::move(operations), std::move(consigned));
	}

private:
	auto disconnect(typename container_type::const_pointer pointer) -> void {
		auto lock = std::scoped_lock{callback_mut};
		callbacks.erase(callbacks.get_iterator(pointer));
	}


	boost::asio::any_io_executor executor;

	container_type callbacks;
	std::shared_mutex callback_mut;
};

} //namespace events

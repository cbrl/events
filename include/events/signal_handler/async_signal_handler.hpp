#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <boost/asio.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

#include <events/connection.hpp>
#include <events/detail/parallel_publish.hpp>


// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {


template<typename FunctionT, typename ExecutorT = boost::asio::any_io_executor, typename AllocatorT = std::allocator<void>>
class async_signal_handler;


/**
 * @brief An asynchronous variant of @ref signal_handler. Callback invocations will happen on an ASIO executor.
 *        Callbacks that don't finish before a new signal is published will still be invoked. An ASIO completion token
 *        may optionally be provided when publishing a signal, which will be invoked once all callbacks have completed.
 *        If the signal returns values, these will be passed to the completion token.
 */
template<typename ReturnT, typename... ArgsT, typename ExecutorT, typename AllocatorT>
class [[nodiscard]] async_signal_handler<ReturnT(ArgsT...), ExecutorT, AllocatorT> {
public:
	using allocator_type = AllocatorT;
	using executor_type = ExecutorT;
	using function_type = ReturnT(ArgsT...);
	using completion_type = std::conditional_t<std::is_same_v<void, ReturnT>, void(), void(std::vector<ReturnT>)>;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using callback_type = std::function<function_type>;
	using callback_ptr = std::shared_ptr<callback_type>;

	using snapshot_element_alloc_type = typename alloc_traits::template rebind_alloc<callback_ptr>;
	using snapshot_type = std::vector<callback_ptr, snapshot_element_alloc_type>;
	using snapshot_ptr = std::shared_ptr<snapshot_type const>;

	// Wrapper for callback operation invocation for use with parallel_group
	struct callback_op {
		ExecutorT exec;
		callback_ptr callback;
		std::shared_ptr<std::tuple<ArgsT...> const> args_payload;

		using executor_type = ExecutorT;
		auto get_executor() const -> executor_type { return exec; }

		template<typename Handler>
		auto operator()(Handler&& handler) && -> void {
			auto execute = [callback = std::move(callback), args_payload = std::move(args_payload), handler = std::forward<Handler>(handler)]() mutable {
				if constexpr (std::same_as<void, ReturnT>) {
					std::apply(*callback, *args_payload);
					std::move(handler)(std::monostate{});
				}
				else {
					auto result = std::apply(*callback, *args_payload);
					std::move(handler)(std::move(result));
				}
			};
			boost::asio::post(exec, std::move(execute));
		}
	};

public:
	async_signal_handler(ExecutorT const& exec) : executor(exec) {
	}

	template<typename ExecutionContext>
	requires std::convertible_to<ExecutionContext&, boost::asio::execution_context&>
	async_signal_handler(ExecutionContext& context) :
		async_signal_handler(context.get_executor()) {
	}

	async_signal_handler(ExecutorT const& exec, AllocatorT const& alloc) :
		allocator(alloc),
		executor(exec) {
	}

	template<typename ExecutionContext>
	requires std::convertible_to<ExecutionContext&, boost::asio::execution_context&>
	async_signal_handler(ExecutionContext& context, AllocatorT const& alloc) :
		async_signal_handler(context.get_executor(), alloc) {
	}

	/// Construct a new async_signal_handler that holds the same callbacks as another
	async_signal_handler(async_signal_handler const& other) {
		auto lock = std::scoped_lock{other.mutex};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		executor = other.executor;
		snapshot = other.snapshot;
	}

	/// Construct a new async_signal_handler that holds the same callbacks as another
	async_signal_handler(async_signal_handler const& other, AllocatorT const& alloc) :
		allocator(alloc) {
		auto lock = std::scoped_lock{other.mutex};
		executor = other.executor;
		snapshot = other.snapshot;
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 * @details Moving from a handler with running callbacks is undefined behavior.
	 */
	async_signal_handler(async_signal_handler&& other) {
		auto lock = std::scoped_lock{other.mutex};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		executor = std::move(other.executor);
		snapshot = std::move(other.snapshot);
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 * @details Moving from a handler with running callbacks is undefined behavior.
	 */
	async_signal_handler(async_signal_handler&& other, AllocatorT const& alloc) :
		allocator(alloc) {
		auto lock = std::scoped_lock{other.mutex};
		executor = std::move(other.executor);
		snapshot = other.snapshot; // Can't move since we need to use our own allocator
	}

	~async_signal_handler() = default;

	/**
	 * @brief Copy the valid callbacks from an async_signal_handler to this one
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior
	 */
	auto operator=(async_signal_handler const& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto lock1 = std::scoped_lock{mutex, other.mutex};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		executor = other.executor;
		snapshot = other.snapshot;

		return *this;
	}

	/**
	 * @brief Move the valid callbacks from an async_signal_handler to this one
	 *
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior, and moving from
	 *          one that has running callbacks is undefined behavior.
	 */
	auto operator=(async_signal_handler&& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto lock1 = std::scoped_lock{mutex, other.mutex};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		executor = std::move(other.executor);
		snapshot = std::move(other.snapshot);

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
	}

	/// Get the executor associated with this object
	[[nodiscard]]
	auto get_executor() const -> executor_type {
		return executor;
	}

	/// Get the number of callbacks registered with this signal handler
	[[nodiscard]]
	auto size() const -> size_t {
		auto lock = std::scoped_lock{mutex};
		return snapshot ? snapshot->size() : 0;
	}

	/// Disconnect all callbacks
	auto disconnect_all() -> void {
		auto lock = std::scoped_lock{mutex};
		snapshot.reset();
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
		auto cb = std::allocate_shared<callback_type>(allocator, std::forward<FunctionT>(func));
		auto* raw = cb.get();

		{
			auto lock = std::scoped_lock{mutex};
			auto new_snap = copy_snapshot();
			new_snap.push_back(std::move(cb));
			snapshot = make_snapshot(std::move(new_snap));
		}

		return connection{[this, raw] { disconnect(raw); }};
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		auto snap = acquire_snapshot();
		if (!snap) {
			return;
		}

		for (auto const& cb : *snap) {
			(*cb)(args...);
		}
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires(!std::same_as<void, ReturnT>)
	{
		auto snap = acquire_snapshot();

		auto results = std::vector<ReturnT>{};
		if (!snap) {
			return results;
		}

		results.reserve(snap->size());

		for (auto const& cb : *snap) {
			results.emplace_back((*cb)(args...));
		}

		return results;
	}

	/**
	 * @brief Fire the signal asynchronously
	 *
	 * @param args The signal arguments
	 */
	auto async_publish(ArgsT... args) -> void {
		auto args_payload = std::allocate_shared<const std::tuple<ArgsT...>>(
			allocator,
			std::forward<ArgsT>(args)...
		);

		auto snap = acquire_snapshot();
		if (!snap) {
			return;
		}

		for (auto const& cb : *snap) {
			boost::asio::post(executor, [cb, args_payload]() {
				(void)std::apply(*cb, *args_payload);
			});
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
		auto args_payload = std::allocate_shared<std::tuple<ArgsT...> const>(
			allocator,
			std::forward<ArgsT>(args)...
		);

		using operations_allocator_type = typename alloc_traits::template rebind_alloc<callback_op>;
		auto operations = std::vector<callback_op, operations_allocator_type>{operations_allocator_type{allocator}};

		auto snap = acquire_snapshot();
		if (!snap) {
			// No callbacks, complete immediately with empty results
			if constexpr (std::same_as<void, ReturnT>) {
				boost::asio::post(executor, std::forward<CompletionToken>(completion));
			} else {
				boost::asio::post(executor, [completion = std::forward<CompletionToken>(completion)]() mutable {
					std::move(completion)(std::vector<ReturnT>{});
				});
			}
			return;
		}

		operations.reserve(snap->size());

		// Create a callback operation for each registered callback
		for (auto const& cb : *snap) {
			operations.emplace_back(executor, cb, args_payload);
		}

		// Initiate the callbacks as a parallel_group, with a completion that takes either nothing if ReturnT is void,
		// or a vector of the callback results otherwise.
		return detail::parallel_publish<completion_type>(
			executor,
			std::move(operations),
			std::forward<CompletionToken>(completion),
			allocator
		);
	}

private:
	auto disconnect(callback_type* raw) -> void {
		auto lock = std::scoped_lock{mutex};
		if (!snapshot) {
			return;
		}

		auto new_snap = snapshot_type{snapshot_element_alloc_type{allocator}};
		new_snap.reserve(snapshot->size());

		bool found = false;

		for (auto const& cb : *snapshot) {
			if (!found && cb.get() == raw) {
				found = true;
				continue;
			}
			new_snap.push_back(cb);
		}

		if (!found) {
			return;
		}

		snapshot = new_snap.empty() ? nullptr : make_snapshot(std::move(new_snap));
	}

	[[nodiscard]]
	auto acquire_snapshot() const -> snapshot_ptr {
		auto lock = std::scoped_lock{mutex};
		return snapshot;
	}

	[[nodiscard]]
	auto copy_snapshot() const -> snapshot_type {
		if (snapshot) {
			return snapshot_type{*snapshot, snapshot_element_alloc_type{allocator}};
		}
		return snapshot_type{snapshot_element_alloc_type{allocator}};
	}

	[[nodiscard]]
	auto make_snapshot(snapshot_type snap) const -> snapshot_ptr {
		return std::allocate_shared<snapshot_type>(allocator, std::move(snap));
	}

	AllocatorT allocator;
	ExecutorT executor;
	snapshot_ptr snapshot;
	mutable std::mutex mutex;
};

}  //namespace events

// NOLINTEND(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)

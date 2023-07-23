#pragma once

#include <events/detail/parallel_group.hpp>


// The following file is a re-implementation of asio::experimental::parallel_group optimzed for the
// case where completion order does not need to be tracked and no results need to be passed to the
// completion handler. That is to say: the completion handler should take no arguments.

// NOLINTBEGIN

namespace events::detail {

template <typename Handler, typename Op>
struct void_ranged_parallel_group_completion_handler {
	using executor_type = std::decay_t<
		typename boost::asio::prefer_result<
			boost::asio::associated_executor_t<Handler>,
			boost::asio::execution::outstanding_work_t::tracked_t
		>::type
	>;

	void_ranged_parallel_group_completion_handler(Handler&& h) :
		handler(std::move(h)),
		executor(
			boost::asio::prefer(
				boost::asio::get_associated_executor(handler),
				boost::asio::execution::outstanding_work.tracked
			)
		) {
	}

	auto get_executor() const noexcept -> executor_type {
		return executor;
	}

	auto operator()() -> void {
		std::move(handler)();
	}

	Handler handler;
	executor_type executor;
};


template <typename Condition, typename Handler, typename Op, typename Allocator>
struct void_ranged_parallel_group_state {
	using handler_type = typename boost::asio::prefer_result<
		boost::asio::associated_executor_t<Handler>,
		boost::asio::execution::outstanding_work_t::tracked_t
	>::type;

	void_ranged_parallel_group_state(Condition&& c, Handler&& h, size_t size, Allocator const& allocator) :
		cancellations_requested(size),
		outstanding(size),
		cancellation_signals(size, rebind_alloc<Allocator, boost::asio::cancellation_signal>(allocator)),
		cancellation_condition(std::move(c)),
		handler(std::move(h)) {
	}

	// The non-none cancellation type that resulted from a cancellation condition.
	// Stored here for use by the group's initiating function.
	std::atomic<boost::asio::cancellation_type_t> cancel_type{boost::asio::cancellation_type::none};

	// The number of cancellations that have been requested, either on completion
	// of the operations within the group, or via the cancellation slot for the
	// group operation. Initially set to the number of operations to prevent
	// cancellation signals from being emitted until after all of the group's
	// operations' initiating functions have completed.
	std::atomic_size_t cancellations_requested;

	// The number of operations that are yet to complete. Used to determine when
	// it is safe to invoke the user's completion handler.
	std::atomic_size_t outstanding;

	// The cancellation signals for each operation in the group.
	std::vector<boost::asio::cancellation_signal, rebind_alloc<Allocator, boost::asio::cancellation_signal>> cancellation_signals;

	// The cancellation condition is used to determine whether the results from an
	// individual operation warrant a cancellation request for the whole group.
	Condition cancellation_condition;

	// The proxy handler to be invoked once all operations in the group complete.
	void_ranged_parallel_group_completion_handler<Handler, Op> handler;
};


template <typename Condition, typename Handler, typename Op, typename Allocator>
struct void_ranged_parallel_group_op_handler {
	using cancellation_slot_type = boost::asio::cancellation_slot;

	void_ranged_parallel_group_op_handler(
		std::shared_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> group_state,
		size_t op_idx
	) :
		state(std::move(group_state)),
		idx(op_idx) {
	}

	auto get_cancellation_slot() const noexcept -> cancellation_slot_type {
		return state->cancellation_signals[idx].slot();
	}

	auto operator()(auto&&... args) -> void {
		// Determine whether the results of this operation require cancellation of
		// the whole group.
		boost::asio::cancellation_type_t cancel_type = state->cancellation_condition(args...);

		if (cancel_type != boost::asio::cancellation_type::none) {
			// Save the type for potential use by the group's initiating function.
			state->cancel_type = cancel_type;

			// If we are the first operation to request cancellation, emit a signal
			// for each operation in the group.
			if (state->cancellations_requested++ == 0)
			for (size_t i = 0; i < state->cancellation_signals.size(); ++i) {
				if (i != idx) {
					state->cancellation_signals[i].emit(cancel_type);
				}
			}
		}

		// If this is the last outstanding operation, invoke the user's handler.
		if (--state->outstanding == 0) {
			boost::asio::dispatch(std::move(state->handler));
		}
	}

	std::shared_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> state;
	size_t idx;
};


template <typename Executor, typename Condition, typename Handler, typename Op, typename Allocator>
struct void_ranged_parallel_group_op_handler_with_executor : void_ranged_parallel_group_op_handler<Condition, Handler, Op, Allocator> {
	using base_type = void_ranged_parallel_group_op_handler<Condition, Handler, Op, Allocator>;
	using cancellation_slot_type = boost::asio::cancellation_slot;
	using executor_type = Executor;

	// Proxy handler that forwards the emitted signal to the correct executor.
	struct cancel_proxy {
		cancel_proxy(
			std::shared_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> state,
			executor_type ex
		) :
			weak_state(std::move(state)),
			executor(std::move(ex)) {
		}

		auto operator()(boost::asio::cancellation_type_t type) -> void {
			if (auto state = weak_state.lock()) {
				boost::asio::dispatch(executor, [state, sig = &signal, type]{ sig->emit(type); });
			}
		}

		std::weak_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> weak_state;
		boost::asio::cancellation_signal signal;
		executor_type executor;
	};

	void_ranged_parallel_group_op_handler_with_executor(
		std::shared_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> group_state,
		executor_type ex,
		size_t op_idx
	) : void_ranged_parallel_group_op_handler<Condition, Handler, Op, Allocator>(std::move(group_state), op_idx) {

		cancel = &this->state->cancellation_signals[op_idx].slot().template emplace<cancel_proxy>(
			this->state,
			std::move(ex)
		);
	}

	auto get_cancellation_slot() const noexcept -> cancellation_slot_type {
		return cancel->signal.slot();
	}

	auto get_executor() const noexcept -> executor_type {
		return cancel->executor;
	}

	cancel_proxy* cancel;
};


template <typename Condition, typename Handler, typename Op, typename Allocator>
struct void_ranged_parallel_group_cancellation_handler {
	void_ranged_parallel_group_cancellation_handler(
		std::shared_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> state
	) : weak_state(std::move(state)) {
	}

	auto operator()(boost::asio::cancellation_type_t cancel_type) -> void {
		// If we are the first place to request cancellation, i.e. no operation has
		// yet completed and requested cancellation, emit a signal for each
		// operation in the group.
		if (cancel_type != boost::asio::cancellation_type::none) {
			if (auto state = weak_state.lock()) {
				if (state->cancellations_requested++ == 0) {
					for (size_t i = 0; i < state->cancellation_signals.size(); ++i) {
						state->cancellation_signals[i].emit(cancel_type);
					}
				}
			}
		}
	}

	std::weak_ptr<void_ranged_parallel_group_state<Condition, Handler, Op, Allocator>> weak_state;
};


template <typename Condition, typename Handler, typename Range, typename Allocator>
auto void_ranged_parallel_group_launch(
	Condition cancellation_condition,
    Handler handler,
	Range&& range,
	Allocator const& allocator
) -> void {
	// Get the user's completion handler's cancellation slot, so that we can allow
	// cancellation of the entire group.
	auto slot = boost::asio::get_associated_cancellation_slot(handler);

	// The type of the asynchronous operation.
	using op_type = typename std::decay_t<decltype(*std::declval<typename Range::iterator>())>;

	// Create the shared state for the operation.
	using state_type = void_ranged_parallel_group_state<Condition, Handler, op_type, Allocator>;
	auto state = std::allocate_shared<state_type>(
		boost::asio::detail::recycling_allocator<state_type, boost::asio::detail::thread_info_base::parallel_group_tag>(),
		std::move(cancellation_condition),
		std::move(handler),
		range.size(),
		allocator
	);

	size_t idx = 0;
	for (auto&& op : std::forward<Range>(range)) {
		using ex_type = boost::asio::associated_executor_t<op_type>;
		ex_type ex = boost::asio::get_associated_executor(op);
		std::move(op)(
			void_ranged_parallel_group_op_handler_with_executor<ex_type, Condition, Handler, op_type, Allocator>(
				state,
				std::move(ex),
				idx++
			)
		);
	}

	// Check if any of the operations has already requested cancellation, and if
	// so, emit a signal for each operation in the group.
	if ((state->cancellations_requested -= range.size()) > 0) {
		for (auto& signal : state->cancellation_signals) {
			signal.emit(state->cancel_type);
		}
	}

	// Register a handler with the user's completion handler's cancellation slot.
	if (slot.is_connected()) {
		slot.template emplace<void_ranged_parallel_group_cancellation_handler<Condition, Handler, op_type, Allocator>>(state);
	}
}


template <typename Range, typename Allocator = std::allocator<void>>
class void_ranged_parallel_group {
private:
	struct initiate_async_wait {
		template <typename Handler, typename Condition>
		auto operator()(Handler&& h, Condition&& c, Range&& range, Allocator const& allocator) const -> void {
			void_ranged_parallel_group_launch(
				std::move(c),
				std::move(h),
				std::forward<Range>(range),
				allocator
			);
		}
	};

public:
	/// The completion signature for the group of operations.
	using signature = void();

	/// Constructor.
	explicit void_ranged_parallel_group(Range op_range, Allocator const& alloc = Allocator()) :
		range(std::move(op_range)),
		allocator(alloc) {
	}

	/// Initiate an asynchronous wait for the group of operations.
	/**
	 * Launches the group and asynchronously waits for completion.
	 *
	 * @param cancellation_condition A function object, called on completion of
	 * an operation within the group, that is used to determine whether to cancel
	 * the remaining operations. The function object is passed the arguments of
	 * the completed operation's handler. To trigger cancellation of the remaining
	 * operations, it must return a boost::asio::cancellation_type value other
	 * than <tt>boost::asio::cancellation_type::none</tt>.
	 *
	 * @param token A @ref completion_token whose signature is comprised of
	 * a @c std::vector<size_t, Allocator> indicating the completion order of
	 * the operations, followed by a vector for each of the completion signature's
	 * arguments.
	 *
	 * The library provides the following @c cancellation_condition types:
	 *
	 * @li boost::asio::experimental::wait_for_all
	 * @li boost::asio::experimental::wait_for_one
	 * @li boost::asio::experimental::wait_for_one_error
	 * @li boost::asio::experimental::wait_for_one_success
	 */
	template <typename CancellationCondition, boost::asio::completion_token_for<signature> CompletionToken>
	auto async_wait(CancellationCondition cancellation_condition, CompletionToken&& token) {
		return boost::asio::async_initiate<CompletionToken, signature>(
			initiate_async_wait(),
			token,
			std::move(cancellation_condition),
			std::move(range),
			allocator
		);
	}

	Range range;
	Allocator allocator;
};


template <typename Range>
requires (boost::asio::experimental::is_async_operation_range<std::decay_t<Range>>::value != 0)
[[nodiscard]]
auto make_void_parallel_group(Range&& range) -> void_ranged_parallel_group<std::decay_t<Range>> {
	return void_ranged_parallel_group<std::decay_t<Range>>(std::forward<Range>(range));
}

template <typename Range, typename Allocator>
requires (boost::asio::experimental::is_async_operation_range<std::decay_t<Range>>::value != 0)
[[nodiscard]]
auto make_void_parallel_group(Range&& range, Allocator const& allocator) -> void_ranged_parallel_group<std::decay_t<Range>> {
	return void_ranged_parallel_group<std::decay_t<Range>>(std::forward<Range>(range), allocator);
}

} //namespace events::detail

// NOLINTEND

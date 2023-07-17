#pragma once

#include <concepts>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <plf_colony.h>

#include <events/connection.hpp>


namespace events {

template<typename FunctionT, typename AllocatorT = std::allocator<void>>
class synchronized_signal_handler;


/**
 * @brief A thread-safe @ref signal_handler
 */
template<typename ReturnT, typename... ArgsT, typename AllocatorT>
class [[nodiscard]] synchronized_signal_handler<ReturnT(ArgsT...), AllocatorT> {
public:
	using function_type = ReturnT(ArgsT...);
	using allocator_type = AllocatorT;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using element_type = std::function<function_type>;
	using container_allocator_type = typename alloc_traits::template rebind_alloc<element_type>;
	using container_type = plf::colony<element_type, container_allocator_type>;

	using erase_element_type = typename container_type::const_pointer;
	using erase_container_allocator_type = typename alloc_traits::template rebind_alloc<erase_element_type>;
	using erase_container_type = std::vector<erase_element_type, erase_container_allocator_type>;

public:
	synchronized_signal_handler() = default;

	synchronized_signal_handler(synchronized_signal_handler const& other) {
		auto lock = std::scoped_lock{other.callback_mut};
		callbacks = other.callbacks;
	}

	synchronized_signal_handler(synchronized_signal_handler&& other) {
		auto locks = std::scoped_lock{other.callback_mut, other.erase_mut};
		to_erase = std::move(other.to_erase);
		callbacks = std::move(other.callbacks);
		allocator = std::move(other.allocator);
	}

	explicit synchronized_signal_handler(AllocatorT const& alloc) :
		allocator(alloc),
		callbacks(allocator),
		to_erase(allocator) {
	}

	synchronized_signal_handler(synchronized_signal_handler const& other, AllocatorT const& alloc) :
		allocator(alloc),
		to_erase(allocator) {

		auto lock = std::scoped_lock{other.callback_mut};
		callbacks = container_type{other.callbacks, allocator};
	}

	synchronized_signal_handler(synchronized_signal_handler&& other, AllocatorT const& alloc) : allocator(alloc) {
		auto locks = std::scoped_lock{other.callback_mut, other.erase_mut};
		to_erase = erase_container_type{std::move(other.to_erase), allocator};
		callbacks = container_type{std::move(other.callbacks), allocator};
	}

	~synchronized_signal_handler() = default;

	auto operator=(synchronized_signal_handler const& other) -> synchronized_signal_handler& {
		auto locks = std::scoped_lock{callback_mut, other.callback_mut};
		callbacks = other.callbacks;
		return *this;
	}

	auto operator=(synchronized_signal_handler&& other) -> synchronized_signal_handler& {
		auto locks = std::scoped_lock{callback_mut, erase_mut, other.callback_mut, other.erase_mut};
		callbacks = std::move(other.callbacks);
		to_erase = std::move(other.to_erase);
		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
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
	template<std::invocable<ArgsT...> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		auto lock = std::unique_lock{callback_mut};
		auto const it = callbacks.insert(std::forward<FunctionT>(callback));
		lock.unlock();

		return connection{[this, ptr = &(*it)] { disconnect(ptr); }};
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		erase_expired_callbacks();

		auto lock = std::shared_lock{callback_mut};
		for (auto& callback : callbacks) {
			callback(args...);
		}
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires(!std::same_as<void, ReturnT>)
	{
		erase_expired_callbacks();

		auto lock = std::shared_lock{callback_mut};

		auto results = std::vector<ReturnT>{};
		results.reserve(callbacks.size());

		for (auto& callback : callbacks) {
			results.emplace_back(callback(args...));
		}

		return results;
	}

	/// Disconnect all callbacks
	auto disconnect() -> void {
		auto lock = std::scoped_lock{callback_mut};
		callbacks.clear();
		to_erase.clear();
	}

private:
	auto disconnect(typename container_type::const_pointer callback_pointer) -> void {
		auto lock = std::scoped_lock{erase_mut};
		to_erase.push_back(callback_pointer);
	}

	auto erase_expired_callbacks() -> void {
		auto locks = std::scoped_lock{callback_mut, erase_mut};

		for (auto callback_pointer : to_erase) {
			callbacks.erase(callbacks.get_iterator(callback_pointer));
		}

		to_erase.clear();
	}


	AllocatorT allocator;

	container_type callbacks{allocator};
	std::shared_mutex callback_mut;

	erase_container_type to_erase{allocator};
	std::mutex erase_mut;
};

}  //namespace events

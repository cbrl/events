#pragma once

#include <concepts>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <vector>

#include <plf_colony.h>


namespace events {

template<typename...>
class synchronized_signal_handler;


/**
 * @brief A thread-safe @ref signal_handler
 */
template<typename ReturnT, typename... ArgsT>
class [[nodiscard]] synchronized_signal_handler<ReturnT(ArgsT...)> {
	using container_type = plf::colony<std::function<ReturnT(ArgsT...)>>;

public:
	using function_type = ReturnT(ArgsT...);

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
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT> {
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
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires (!std::same_as<void, ReturnT>) {
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

	container_type callbacks;
	std::shared_mutex callback_mut;

	std::vector<typename container_type::const_pointer> to_erase;
	std::mutex erase_mut;
};

} //namespace events

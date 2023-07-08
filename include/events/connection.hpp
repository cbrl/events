#pragma once

#include <functional>
#include <utility>


namespace events {

class connection {
	template<typename...>
	friend class signal_handler;

	template<typename...>
	friend class synchronized_signal_handler;

	template<typename...>
	friend class async_signal_handler;

	explicit connection(std::function<void()> function) : disconnect_function(std::move(function)) {
	}

public:
	connection() = default;
	connection(connection const&) = default;
	connection(connection&&) noexcept = default;

	~connection() = default;

	auto operator=(connection const&) -> connection& = default;
	auto operator=(connection&&) noexcept -> connection& = default;

	[[nodiscard]]
	explicit operator bool() const noexcept {
		return static_cast<bool>(disconnect_function);
	}

	auto disconnect() -> void {
		if (disconnect_function) {
			disconnect_function();
			disconnect_function = nullptr;
		}
	}

private:
	std::function<void()> disconnect_function;
};


class [[nodiscard]] scoped_connection {
public:
	scoped_connection() = default;
	scoped_connection(scoped_connection const&) = delete;
	scoped_connection(scoped_connection&&) noexcept = default;

	explicit scoped_connection(connection other) : connect(std::move(other)) {
	}

	~scoped_connection() {
		disconnect();
	}

	auto operator=(scoped_connection const&) -> scoped_connection& = delete;
	auto operator=(scoped_connection&&) noexcept -> scoped_connection& = default;

	auto operator=(connection const& other) noexcept -> scoped_connection& {
		connect = other;
		return *this;
	}

	[[nodiscard]]
	explicit operator bool() const noexcept {
		return static_cast<bool>(connect);
	}

	auto disconnect() -> void {
		connect.disconnect();
	}

private:
	connection connect;
};

} //namespace events

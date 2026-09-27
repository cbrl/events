#pragma once

#include <cstdint>
#include <memory>
#include <utility>


namespace events {
namespace detail {

/// Implemented by the shared state of each signal handler, so that connections can refer to it without knowing its type
class slot_owner {
public:
	slot_owner() = default;
	slot_owner(slot_owner const&) = delete;
	slot_owner(slot_owner&&) = delete;

	virtual ~slot_owner() = default;

	auto operator=(slot_owner const&) -> slot_owner& = delete;
	auto operator=(slot_owner&&) -> slot_owner& = delete;

	virtual auto disconnect(std::uint64_t id) noexcept -> void = 0;
	[[nodiscard]] virtual auto connected(std::uint64_t id) const noexcept -> bool = 0;
};

}  //namespace detail


/**
 * @brief A handle to a callback registered with a signal handler or event dispatcher
 *
 * @details Copies of a connection refer to the same callback. A connection only holds a weak reference to its signal
 *          handler, so it may safely outlive it: once the handler is destroyed, disconnect() does nothing and
 *          connected() returns false. Callback ids are never reused, so a stale connection can't affect a callback
 *          that was connected later.
 */
class connection {
	template<typename, typename>
	friend class signal_handler;

	template<typename, typename>
	friend class synchronized_signal_handler;

	connection(std::weak_ptr<detail::slot_owner> slot_owner_ptr, std::uint64_t slot_id) noexcept :
		owner(std::move(slot_owner_ptr)),
		id(slot_id) {
	}

public:
	connection() = default;
	connection(connection const&) = default;

	connection(connection&& other) noexcept : owner(std::move(other.owner)), id(std::exchange(other.id, 0)) {
	}

	~connection() = default;

	auto operator=(connection const&) -> connection& = default;

	auto operator=(connection&& other) noexcept -> connection& {
		owner = std::move(other.owner);
		id = std::exchange(other.id, 0);
		return *this;
	}

	/// Check if the callback is still connected to a signal handler
	[[nodiscard]] auto connected() const noexcept -> bool {
		auto const locked = owner.lock();
		return locked && locked->connected(id);
	}

	/// Equivalent to connected()
	[[nodiscard]] explicit operator bool() const noexcept {
		return connected();
	}

	/// Disconnect the callback. Does nothing if it's already disconnected or its signal handler no longer exists.
	auto disconnect() noexcept -> void {
		if (auto const locked = owner.lock()) {
			locked->disconnect(id);
		}
		owner.reset();
		id = 0;
	}

private:
	std::weak_ptr<detail::slot_owner> owner;
	std::uint64_t id = 0;
};


/// A connection which disconnects its callback when destroyed
class [[nodiscard]] scoped_connection {
public:
	scoped_connection() = default;
	scoped_connection(scoped_connection const&) = delete;
	scoped_connection(scoped_connection&&) noexcept = default;

	//NOLINTNEXTLINE(google-explicit-constructor,hicpp-explicit-conversions)
	scoped_connection(connection other) noexcept : conn(std::move(other)) {
	}

	~scoped_connection() {
		disconnect();
	}

	auto operator=(scoped_connection const&) -> scoped_connection& = delete;

	/// Disconnect the currently held connection, then take ownership of the other's connection
	auto operator=(scoped_connection&& other) noexcept -> scoped_connection& {
		if (&other != this) {
			disconnect();
			conn = std::move(other.conn);
		}
		return *this;
	}

	/// Disconnect the currently held connection, then take ownership of the new one
	auto operator=(connection other) noexcept -> scoped_connection& {
		disconnect();
		conn = std::move(other);
		return *this;
	}

	/// Check if the callback is still connected to a signal handler
	[[nodiscard]] auto connected() const noexcept -> bool {
		return conn.connected();
	}

	/// Equivalent to connected()
	[[nodiscard]] explicit operator bool() const noexcept {
		return connected();
	}

	auto disconnect() noexcept -> void {
		conn.disconnect();
	}

	/// Give up ownership of the connection without disconnecting it
	[[nodiscard]] auto release() noexcept -> connection {
		return std::move(conn);
	}

private:
	connection conn;
};

}  //namespace events

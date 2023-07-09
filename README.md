# events

This library provides utilities for event-driven design. The two main components are the `signal_handler`, and the
`event_dispatcher`. The `signal_handler` stores a set of callbacks that will be invoked with the signal's arguments
when that signal is fired. The `event_dispatcher` utilizes signal handlers to allow registration of listener callbacks
for any user-defined event type. These events can be either enqueued for later dispatch or immediately dispatched to
all listeners.

Variants of both the `signal_handler` and `event_dispatcher` are provided for use in single-threaded and multi-threaded
applications. Additionally, a variant of each is provided that integrates with ASIO to allow easily dispatching
listeners to an execution context with support for ASIO's completion tokens.

```cpp
auto sigh = events::signal_handler<void(int)>{};

sigh.connect([](int n) {
	std::cout << std::format("Signal received: {}\n", n);
});

sigh.publish(0);
```

```cpp
struct EntityCreated {
	uint64_t handle;
};

auto dispatcher = events::event_dispatcher{};

dispatcher.connect<EntityCreated>([](auto const& event) {
	std::cout << std::format("New entity: {}\n", event.handle);
});

dispatcher.enqueue(EntityCreated{0});
dispatcher.dispatch();
```

# Building and installing

See the [BUILDING](BUILDING.md) document.

# Contributing

See the [CONTRIBUTING](CONTRIBUTING.md) document.

# Licensing

[MIT License](LICENSE)

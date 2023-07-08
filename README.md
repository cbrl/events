# events

This library provides utilities for event-driven design. The two main components are the `signal_handler`, and the
`event_dispatcher`. The `signal_handler` stores a set of callbacks that will be invoked with the signal's arguments
when that signal is fired. The `event_dispatcher` utilizes signal handlers to allow registration of listener callbacks
for any user-defined event type. These events can be either enqueued for later dispatch or immediately dispatched to
all listeners.

Variants of both the `signal_handler` and `event_dispatcher` are provided for use in single-threaded
and multi-threaded applications. Additionally, a variant of each is provided that integrates with ASIO to allow easily
dispatching listeners to an execution context with support for ASIO's completion tokens.

# Building and installing

See the [BUILDING](BUILDING.md) document.

# Contributing

See the [CONTRIBUTING](CONTRIBUTING.md) document.

# Licensing

<!--
Please go to https://choosealicense.com/licenses/ and choose a license that
fits your needs. The recommended license for a project of this type is the
Boost Software License 1.0.
-->

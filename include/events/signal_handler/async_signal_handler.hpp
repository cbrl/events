#pragma once

#include <memory>

#include <events/detail/async_concurrent_signal_handler.hpp>
#include <events/detail/async_drop_signal_handler.hpp>
#include <events/signal_handler/callback_policy.hpp>


namespace events {

template<typename FunctionT, typename PolicyT, typename AllocatorT = std::allocator<void>>
class async_signal_handler;

}  //namespace events

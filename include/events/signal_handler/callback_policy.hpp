#pragma once

namespace events {

/// Defines the how a callback is handled when a signal is fired while the callback is still executing
struct callback_policy {
	struct drop {};  ///< The callback will drop the signal if it hasn't finished processing the last signal
	struct concurrent {};  ///< The callback will be launched again even if it has finished processing the last signal
};

} //namespace events

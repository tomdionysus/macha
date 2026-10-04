// SPDX-License-Identifier: GPL-3.0-or-later
#include "cluster/control_objects.hpp"

#include "cluster/cluster.hpp"
#include "codec.hpp"
#include "crypto.hpp"
#include "log.hpp"
#include "storage/local_store.hpp"

namespace macha {

ControlObjectFetch::ControlObjectFetch(ClusterNode& node, LocalStore& control)
    : node_(node), control_(control) {}

bool ControlObjectFetch::pull(const ObjectId& id, const Observer& observe) {
    if (control_.valid(id))
        return true;

    Writer writer;
    writer.fixed(id.bytes);
    const auto payload = writer.take();

    size_t asked = 0;
    std::string last_failure = "no active peer";
    for (const auto& target : node_.membership().active()) {
        if (target.id == node_.node_id())
            continue;
        ++asked;
        try {
            auto started = Clock::now();
            auto reply = node_.call(target, MessageType::get_control_object, payload,
                                    FrameType::speculative);
            if (reply.message.type != MessageType::control_object_reply) {
                last_failure = target.host + ": " + message_type_name(reply.message.type);
                continue;
            }

            Reader reader(reply.message.payload);
            ObjectId returned{reader.fixed<32>()};
            auto data = reader.bytes(128 * 1024 * 1024);
            reader.finish();
            if (returned != id || object_id(data) != id) {
                Log::debug("control object read " + target.host + ": integrity failure");
                last_failure = target.host + ": integrity failure";
                continue;
            }
            if (observe)
                observe(data.size(), Clock::now() - started);
            if (control_.put(id, data))
                return true;
            last_failure = "local control store put failed";
        } catch (const std::exception& e) {
            Log::debug("control object read " + target.host + ": " + e.what());
            last_failure = target.host + ": " + e.what();
        }
    }
    Log::warn("control object unavailable id=" + hex(id.bytes) + " peers_asked=" +
              std::to_string(asked) + " last_failure=" + last_failure);
    return false;
}

} // namespace macha

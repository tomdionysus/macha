// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/held_ledger.hpp"

#include "durable_file.hpp"

#include <algorithm>

namespace macha {

namespace {
// One, as eight big-endian bytes.
const Bytes held_value{0, 0, 0, 0, 0, 0, 0, 1};
} // namespace

HeldLedger::HeldLedger(std::filesystem::path dir, std::array<uint8_t, 32> key,
                       ObjectTrie::Options options)
    : dir_(std::move(dir)), trie_(std::make_unique<ObjectTrie>(dir_, key, options)) {
    seeded_.store(std::filesystem::exists(dir_ / "seeded"), std::memory_order_release);
}

std::optional<bool> HeldLedger::held(const ObjectId& id) const {
    if (!seeded())
        return std::nullopt;
    Lock lock(trie_mutex_);
    return trie_->get(id).has_value();
}

std::vector<ObjectId> HeldLedger::held_with_prefix(uint16_t prefix) const {
    const auto first = static_cast<uint8_t>(prefix >> 8);
    const auto second = static_cast<uint8_t>(prefix & 0xff);
    // Start after the last id of the prefix before.
    std::optional<ObjectId> after;
    if (prefix) {
        ObjectId before;
        before.bytes.fill(0xff);
        before.bytes[0] = static_cast<uint8_t>((prefix - 1) >> 8);
        before.bytes[1] = static_cast<uint8_t>((prefix - 1) & 0xff);
        after = before;
    }
    std::vector<ObjectId> out;
    Lock lock(trie_mutex_);
    while (true) {
        const auto page = trie_->next(after, 256);
        for (const auto& [id, value] : page) {
            if (id.bytes[0] != first || id.bytes[1] != second)
                return out;
            out.push_back(id);
        }
        if (page.size() < 256)
            return out;
        after = page.back().first;
    }
}

void HeldLedger::record(const ObjectId& id, bool held) {
    Lock lock(queue_mutex_);
    queued_.push_back({id, held ? std::optional<Bytes>(held_value) : std::nullopt});
}

void HeldLedger::drop_queued() {
    Lock lock(queue_mutex_);
    queued_.clear();
}

void HeldLedger::flush() {
    Lock flushing(flush_mutex_);
    std::vector<ObjectTrie::Change> changes;
    {
        Lock lock(queue_mutex_);
        changes.swap(queued_);
    }
    if (changes.empty())
        return;
    // In order, a later change to an id winning.
    std::stable_sort(changes.begin(), changes.end(),
                     [](const ObjectTrie::Change& a, const ObjectTrie::Change& b) {
                         return a.id < b.id;
                     });
    std::vector<ObjectTrie::Change> unique;
    unique.reserve(changes.size());
    for (const auto& change : changes) {
        if (!unique.empty() && unique.back().id == change.id)
            unique.back() = change;
        else
            unique.push_back(change);
    }
    // The journal's sync is the write; lookups go on meanwhile.
    write_journal(unique);
    Lock lock(trie_mutex_);
    trie_->install(unique);
}

void HeldLedger::write_journal(std::span<const ObjectTrie::Change> changes) {
    trie_->write(changes);
}

void HeldLedger::seed(const std::function<std::vector<ObjectId>()>& snapshot) {
    Lock flushing(flush_mutex_);
    const auto present = snapshot();
    std::vector<ObjectTrie::Record> records;
    records.reserve(present.size());
    for (const auto& id : present)
        records.emplace_back(id, held_value);
    {
        Lock lock(trie_mutex_);
        trie_->replace_all(std::move(records));
    }
    durable_replace_file(dir_ / "seeded", "1\n");
    seeded_.store(true, std::memory_order_release);
}

ObjectTrie::Stats HeldLedger::stats() const {
    Lock lock(trie_mutex_);
    return trie_->stats();
}

uint64_t HeldLedger::size() const {
    Lock lock(trie_mutex_);
    return trie_->size();
}

} // namespace macha

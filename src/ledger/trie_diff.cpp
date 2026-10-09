// SPDX-License-Identifier: GPL-3.0-or-later
#include "ledger/trie_diff.hpp"

#include "codec.hpp"

#include <algorithm>
#include <stdexcept>

namespace macha {

namespace {

// Both sides' records below one prefix, compared in id order.
void compare(const std::vector<ObjectTrie::Record>& left,
             const std::vector<ObjectTrie::Record>& right,
             const std::function<void(const TrieDifference&)>& visit) {
    auto l = left.begin();
    auto r = right.begin();
    while (l != left.end() || r != right.end()) {
        if (r == right.end() || (l != left.end() && l->first < r->first)) {
            visit({l->first, l->second, std::nullopt});
            ++l;
        } else if (l == left.end() || r->first < l->first) {
            visit({r->first, std::nullopt, r->second});
            ++r;
        } else {
            if (l->second != r->second)
                visit({l->first, l->second, r->second});
            ++l;
            ++r;
        }
    }
}

} // namespace

TrieDiffCost diff_tries(TrieSource& left, TrieSource& right,
                        const std::function<void(const TrieDifference&)>& visit,
                        const std::function<void()>& pause) {
    TrieDiffCost cost;
    const auto left_root = left.root();
    const auto right_root = right.root();
    if (left_root == right_root)
        return cost;
    std::vector<ObjectTrie::Prefix> descend;
    std::vector<ObjectTrie::Prefix> read;
    const auto place = [&](const ObjectTrie::Prefix& prefix, const ObjectTrie::Summary& a,
                           const ObjectTrie::Summary& b) {
        if (std::max(a.count, b.count) <= ObjectTrie::leaf_max)
            read.push_back(prefix);
        else
            descend.push_back(prefix);
    };
    place({}, left_root, right_root);
    while (!descend.empty() || !read.empty()) {
        if (!read.empty()) {
            const auto a = left.records(read);
            const auto b = right.records(read);
            ++cost.rounds;
            cost.prefixes += read.size();
            for (size_t i = 0; i < read.size(); ++i) {
                cost.records += a[i].size() + b[i].size();
                compare(a[i], b[i], visit);
            }
            read.clear();
        }
        if (!descend.empty()) {
            const auto level = std::move(descend);
            descend.clear();
            const auto a = left.children(level);
            const auto b = right.children(level);
            ++cost.rounds;
            cost.prefixes += level.size();
            for (size_t i = 0; i < level.size(); ++i)
                for (size_t slot = 0; slot < 256; ++slot)
                    if (a[i][slot] != b[i][slot])
                        place(level[i].child(static_cast<uint8_t>(slot)), a[i][slot],
                              b[i][slot]);
        }
        if (pause)
            pause();
    }
    return cost;
}

namespace {

// One trie read forward a page at a time.
class Cursor {
  public:
    explicit Cursor(const ObjectTrie::Snapshot& snapshot) : snapshot_(snapshot) { fill(); }
    const ObjectTrie::Record* peek() const { return at_ < page_.size() ? &page_[at_] : nullptr; }
    void advance() {
        if (++at_ >= page_.size() && !done_)
            fill();
    }
    // Moves to the first record at or after `id`.
    void seek(const ObjectId& id) {
        while (const auto* record = peek()) {
            if (!(record->first < id))
                return;
            advance();
        }
    }

  private:
    static constexpr size_t page_size = 4096;
    void fill() {
        page_ = snapshot_.next(after_, page_size);
        at_ = 0;
        done_ = page_.size() < page_size;
        if (!page_.empty())
            after_ = page_.back().first;
    }
    const ObjectTrie::Snapshot& snapshot_;
    std::vector<ObjectTrie::Record> page_;
    size_t at_{};
    std::optional<ObjectId> after_;
    bool done_{};
};

} // namespace

void records_not_held(const ObjectTrie::Snapshot& wanted,
                      std::span<const ObjectTrie::Snapshot> held,
                      const std::function<void(const ObjectTrie::Record&)>& visit,
                      const std::function<void()>& pause) {
    Cursor want(wanted);
    std::vector<Cursor> have;
    have.reserve(held.size());
    for (const auto& snapshot : held)
        have.emplace_back(snapshot);
    size_t seen = 0;
    while (const auto* record = want.peek()) {
        bool found = false;
        for (auto& cursor : have) {
            cursor.seek(record->first);
            if (const auto* next = cursor.peek(); next && next->first == record->first)
                found = true;
        }
        if (!found)
            visit(*record);
        want.advance();
        if (pause && ++seen % 4096 == 0)
            pause();
    }
}

namespace {

void write_prefix(Writer& writer, const ObjectTrie::Prefix& prefix) {
    writer.u8(prefix.length);
    for (size_t i = 0; i < prefix.length; ++i)
        writer.u8(prefix.bytes[i]);
}

ObjectTrie::Prefix read_prefix(Reader& reader) {
    ObjectTrie::Prefix prefix;
    prefix.length = reader.u8();
    if (prefix.length > 32)
        throw DecodeError("trie prefix longer than an id");
    for (size_t i = 0; i < prefix.length; ++i)
        prefix.bytes[i] = reader.u8();
    return prefix;
}

uint32_t read_count(Reader& reader, size_t max, const char* what) {
    const auto count = reader.u32();
    if (count > max)
        throw DecodeError(std::string("too many ") + what);
    return count;
}

} // namespace

Bytes encode_trie_question(const TrieQuestion& question) {
    if (question.prefixes.size() > trie_question_max)
        throw std::invalid_argument("too many prefixes in one trie question");
    Writer writer;
    writer.u8(static_cast<uint8_t>(question.kind));
    writer.u8(static_cast<uint8_t>(question.trie));
    writer.fixed(question.at.bytes);
    writer.u32(static_cast<uint32_t>(question.prefixes.size()));
    for (const auto& prefix : question.prefixes)
        write_prefix(writer, prefix);
    return writer.take();
}

TrieQuestion decode_trie_question(std::span<const uint8_t> encoded) {
    Reader reader(encoded);
    TrieQuestion question;
    const auto kind = reader.u8();
    if (kind > static_cast<uint8_t>(TrieQuestionKind::records))
        throw DecodeError("unknown trie question");
    question.kind = static_cast<TrieQuestionKind>(kind);
    const auto trie = reader.u8();
    if (trie != static_cast<uint8_t>(TrieName::held_data))
        throw DecodeError("unknown trie");
    question.trie = static_cast<TrieName>(trie);
    question.at.bytes = reader.fixed<32>();
    const auto count = read_count(reader, trie_question_max, "prefixes in one trie question");
    question.prefixes.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        question.prefixes.push_back(read_prefix(reader));
        if (question.kind == TrieQuestionKind::children && question.prefixes.back().length >= 32)
            throw DecodeError("a whole id has no children");
    }
    reader.finish();
    return question;
}

Bytes encode_trie_root(const ObjectTrie::Summary& summary) {
    Writer writer;
    writer.u64(summary.count);
    writer.fixed(summary.hash.bytes);
    return writer.take();
}

ObjectTrie::Summary decode_trie_root(std::span<const uint8_t> encoded) {
    Reader reader(encoded);
    ObjectTrie::Summary summary;
    summary.count = reader.u64();
    summary.hash.bytes = reader.fixed<32>();
    reader.finish();
    return summary;
}

Bytes encode_trie_children(std::span<const ObjectTrie::Children> answers) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(answers.size()));
    for (const auto& children : answers) {
        uint16_t present = 0;
        for (const auto& slot : children)
            present += slot.count ? 1 : 0;
        writer.u16(present);
        for (size_t slot = 0; slot < children.size(); ++slot) {
            if (!children[slot].count)
                continue;
            writer.u8(static_cast<uint8_t>(slot));
            writer.u64(children[slot].count);
            writer.fixed(children[slot].hash.bytes);
        }
    }
    return writer.take();
}

std::vector<ObjectTrie::Children> decode_trie_children(std::span<const uint8_t> encoded) {
    Reader reader(encoded);
    const auto count = read_count(reader, trie_question_max, "answers in one trie reply");
    std::vector<ObjectTrie::Children> answers(count);
    for (auto& children : answers) {
        const auto present = reader.u16();
        if (present > 256)
            throw DecodeError("more than 256 trie children");
        int last = -1;
        for (uint16_t i = 0; i < present; ++i) {
            const auto slot = reader.u8();
            if (static_cast<int>(slot) <= last)
                throw DecodeError("trie children out of order");
            last = slot;
            children[slot].count = reader.u64();
            if (!children[slot].count)
                throw DecodeError("an empty trie child sent");
            children[slot].hash.bytes = reader.fixed<32>();
        }
    }
    reader.finish();
    return answers;
}

Bytes encode_trie_records(std::span<const std::vector<ObjectTrie::Record>> answers) {
    Writer writer;
    writer.u32(static_cast<uint32_t>(answers.size()));
    for (const auto& records : answers) {
        writer.u32(static_cast<uint32_t>(records.size()));
        for (const auto& [id, value] : records) {
            writer.fixed(id.bytes);
            writer.bytes(value);
        }
    }
    return writer.take();
}

std::vector<std::vector<ObjectTrie::Record>> decode_trie_records(std::span<const uint8_t> encoded) {
    Reader reader(encoded);
    const auto count = read_count(reader, trie_question_max, "answers in one trie reply");
    std::vector<std::vector<ObjectTrie::Record>> answers(count);
    for (auto& records : answers) {
        const auto size = read_count(reader, ObjectTrie::leaf_max, "records in one trie answer");
        records.reserve(size);
        for (uint32_t i = 0; i < size; ++i) {
            ObjectTrie::Record record;
            record.first.bytes = reader.fixed<32>();
            record.second = reader.bytes(ObjectTrie::value_max);
            if (!records.empty() && !(records.back().first < record.first))
                throw DecodeError("trie records out of order");
            records.push_back(std::move(record));
        }
    }
    reader.finish();
    return answers;
}

} // namespace macha

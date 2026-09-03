#pragma once
#include <unordered_map>
#include <list>
#include <utility>

// A size-bounded cache with true least-recently-used eviction.
//
// Every successful find()/at()/operator[] access promotes that entry to
// "most recently used". Once the number of entries exceeds max_size(),
// the *least* recently used entry is evicted. This is a real fix over a
// pure insertion-order (FIFO) policy: a frequently-reused entry now
// survives, and cold entries are the ones that get pushed out first.
//
// NOT thread-safe. Every method below reads and/or mutates shared state
// (map_, order_, lru_index_), so callers sharing a BoundedCache across
// threads must hold an external lock for the full duration of each call
// -- including while using any reference/iterator it returns, since a
// later emplace()/insert_or_assign() from another thread can evict other
// entries (see the mutex usage notes in image_view.cpp).
template <typename Key, typename T, typename Hash = std::hash<Key>>
class BoundedCache {
public:
    using Map = std::unordered_map<Key, T, Hash>;
    using iterator = typename Map::iterator;
    using const_iterator = typename Map::const_iterator;

    explicit BoundedCache(size_t max_size) : max_size_(max_size) {}

    iterator find(const Key& key) {
        auto it = map_.find(key);
        if (it != map_.end()) touch(key);
        return it;
    }
    const_iterator find(const Key& key) const { return map_.find(key); }

    iterator end() { return map_.end(); }
    const_iterator end() const { return map_.end(); }

    T& at(const Key& key) {
        touch(key);
        return map_.at(key);
    }
    const T& at(const Key& key) const { return map_.at(key); }

    T& operator[](const Key& key) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            touch(key);
            return it->second;
        }
        return emplace(key, T{}).first->second;
    }

    template <typename... Args> std::pair<iterator, bool> emplace(const Key& key, Args&&... args) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            touch(key);
            return {it, false};
        }

        auto result = map_.emplace(key, T(std::forward<Args>(args)...));

        order_.push_front(key);
        lru_index_[key] = order_.begin();

        evict_if_needed();

        return result;
    }

    iterator insert_or_assign(const Key& key, T value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            it->second = std::move(value);
            touch(key);
            return it;
        }

        return emplace(key, std::move(value)).first;
    }

    size_t size() const { return map_.size(); }
    size_t max_size() const { return max_size_; }

    // Changes the capacity at runtime. Shrinking evicts immediately
    // (least-recently-used first) down to the new limit.
    void set_max_size(size_t new_max_size) {
        max_size_ = new_max_size;
        evict_if_needed();
    }

    void setMaxSize(size_t new_size) {
        max_size_ = new_size;
        evict_if_needed();
    }

private:
    // Move `key` to the front (most-recently-used end) of order_ in O(1).
    void touch(const Key& key) {
        auto pos = lru_index_.find(key);
        if (pos == lru_index_.end()) return; // shouldn't happen if map_ has the key
        order_.splice(order_.begin(), order_, pos->second);
        // splice() relinks the node in place; it does not invalidate the
        // list iterator, so lru_index_[key] (== pos->second) is still
        // correct and now points at order_.begin().
    }

    void evict_if_needed() {
        while (map_.size() > max_size_) {
            const Key oldest = order_.back(); // copy: we're about to erase its storage
            map_.erase(oldest);
            lru_index_.erase(oldest);
            order_.pop_back();
        }
    }

private:
    size_t max_size_;
    Map map_;

    std::list<Key> order_; // front = most recently used, back = least recently used
    std::unordered_map<Key, typename std::list<Key>::iterator, Hash> lru_index_;
};

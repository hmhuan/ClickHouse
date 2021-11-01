#pragma once
#include <base/StringRef.h>
#include <unordered_map>
#include <list>
#include <atomic>

namespace DB
{

template<typename V>
struct ListNode
{
    std::string key;
    V value;
    bool active_in_map;
};

template <class V>
struct HasSizeMethod
{
private:
    template <class T>
    static auto check(int) -> decltype(std::declval<T>().size(), std::true_type());
    template <class T>
    static std::false_type check(...);

public:
    static constexpr bool value = std::is_same<decltype(check<V>(0)), std::true_type>::value;
};

template <class V>
class SnapshotableHashTable
{
private:

    using ListElem = ListNode<V>;
    using List = std::list<ListElem>;
    using IndexMap = std::unordered_map<StringRef, typename List::iterator, StringRefHash>;

    List list;
    IndexMap map;
    bool snapshot_mode{false};

    UInt64 approximate_data_size{0};

    enum OperationType
    {
        INSERT = 0,
        INSERT_OR_REPLACE = 1,
        ERASE = 2,
        UPDATE_VALUE = 3,
        GET_VALUE = 4,
        FIND = 5,
        CONTAINS = 6,
        CLEAR = 7,
        CLEAR_OUTDATED_NODES = 8
    };

    /// Update hash table approximate data size
    ///    op_type: operation type
    ///    key_size: key size
    ///    value_size: size of value to add
    ///    old_value_size: size of value to minus
    /// old_value_size=0 means there is no old value with the same key.
    void inline updateDataSize(OperationType op_type, UInt64 key_size, UInt64 value_size, UInt64 old_value_size)
    {
        switch (op_type)
        {
            case INSERT:
                approximate_data_size += key_size;
                approximate_data_size += value_size;
                break;
            case INSERT_OR_REPLACE:
                /// replace
                if (old_value_size)
                {
                    approximate_data_size += key_size;
                    approximate_data_size += value_size;
                    if (!snapshot_mode)
                    {
                        approximate_data_size += key_size;
                        approximate_data_size -= old_value_size;
                    }
                }
                /// inseert
                else
                {
                    approximate_data_size += key_size;
                    approximate_data_size += value_size;
                }
                break;
            case UPDATE_VALUE:
                approximate_data_size += key_size;
                approximate_data_size += value_size;
                if (!snapshot_mode)
                {
                    approximate_data_size -= key_size;
                    approximate_data_size -= old_value_size;
                }
                break;
            case ERASE:
                if (!snapshot_mode)
                {
                    approximate_data_size -= key_size;
                    approximate_data_size -= old_value_size;
                }
                break;
            case CLEAR:
                approximate_data_size = 0;
                break;
            case CLEAR_OUTDATED_NODES:
                approximate_data_size -= key_size;
                approximate_data_size -= value_size;
                break;
            default:
                break;
        }
    }

    /// Calculate object memory size.
    /// @return size(), if T has method size(), otherwise return sizeof(T)
    template <typename T>
    inline UInt64 sizeOf(const typename std::enable_if<HasSizeMethod<T>::value, T>::type * obj)
    {
        return obj->size();
    }

    template <typename T>
    inline UInt64 sizeOf(const typename std::enable_if<!HasSizeMethod<T>::value, T>::type *)
    {
        return sizeof(T);
    }

public:

    using iterator = typename List::iterator;
    using const_iterator = typename List::const_iterator;
    using reverse_iterator = typename List::reverse_iterator;
    using const_reverse_iterator = typename List::const_reverse_iterator;
    using ValueUpdater = std::function<void(V & value)>;

    bool insert(const std::string & key, const V & value)
    {
        auto it = map.find(key);
        if (it == map.end())
        {
            ListElem elem{key, value, true};
            auto itr = list.insert(list.end(), elem);
            map.emplace(itr->key, itr);
            updateDataSize(INSERT, sizeOf<std::string>(&key), sizeOf<V>(&value), 0);
            return true;
        }

        return false;
    }


    void insertOrReplace(const std::string & key, const V & value)
    {
        auto it = map.find(key);
        UInt64 old_value_size = it == map.end() ? 0 : sizeOf<V>(&it->second->value);

        if (it == map.end())
        {
            ListElem elem{key, value, true};
            auto itr = list.insert(list.end(), elem);
            map.emplace(itr->key, itr);
        }
        else
        {
            auto list_itr = it->second;
            if (snapshot_mode)
            {
                ListElem elem{key, value, true};
                list_itr->active_in_map = false;
                auto new_list_itr = list.insert(list.end(), elem);
                map.erase(it);
                map.emplace(new_list_itr->key, new_list_itr);
            }
            else
            {
                list_itr->value = value;
            }
        }
        updateDataSize(INSERT_OR_REPLACE, sizeOf<std::string>(&key), sizeOf<V>(&value), old_value_size);
    }

    bool erase(const std::string & key)
    {
        auto it = map.find(key);
        if (it == map.end())
            return false;

        auto list_itr = it->second;
        if (snapshot_mode)
        {
            list_itr->active_in_map = false;
            map.erase(it);
        }
        else
        {
            map.erase(it);
            list.erase(list_itr);
        }

        updateDataSize(ERASE, sizeOf<std::string>(&key), 0, sizeOf<V>(&list_itr->value));
        return true;
    }

    bool contains(const std::string & key) const
    {
        return map.find(key) != map.end();
    }

    const_iterator updateValue(const std::string & key, ValueUpdater updater)
    {
        auto it = map.find(key);
        assert(it != map.end());

        auto list_itr = it->second;
        UInt64 old_value_size = sizeOf<V>(&list_itr->value);

        const_iterator ret;

        if (snapshot_mode)
        {
            auto elem_copy = *(list_itr);
            list_itr->active_in_map = false;
            map.erase(it);
            updater(elem_copy.value);
            auto itr = list.insert(list.end(), elem_copy);
            map.emplace(itr->key, itr);
            ret = itr;
        }
        else
        {
            updater(list_itr->value);
            ret = list_itr;
        }
        updateDataSize(UPDATE_VALUE, sizeOf<std::string>(&key), sizeOf<V>(&ret->value), old_value_size);
        return ret;
    }

    const_iterator find(const std::string & key) const
    {
        auto map_it = map.find(key);
        if (map_it != map.end())
            return map_it->second;
        return list.end();
    }

    const V & getValue(const std::string & key) const
    {
        auto it = map.find(key);
        assert(it != map.end());
        return it->second->value;
    }

    void clearOutdatedNodes()
    {
        auto start = list.begin();
        auto end = list.end();
        for (auto itr = start; itr != end;)
        {
            if (!itr->active_in_map)
            {
                updateDataSize(CLEAR_OUTDATED_NODES, sizeOf<String>(&itr->key), sizeOf<V>(&itr->value), 0);
                itr = list.erase(itr);
            }
            else
                itr++;
        }
    }

    void clear()
    {
        list.clear();
        map.clear();
        updateDataSize(CLEAR, 0, 0, 0);
    }

    void enableSnapshotMode()
    {
        snapshot_mode = true;
    }

    void disableSnapshotMode()
    {
        snapshot_mode = false;
    }

    size_t size() const
    {
        return map.size();
    }

    size_t snapshotSize() const
    {
        return list.size();
    }

    UInt64 getApproximateSataSize() const
    {
        return approximate_data_size;
    }

    iterator begin() { return list.begin(); }
    const_iterator begin() const { return list.cbegin(); }
    iterator end() { return list.end(); }
    const_iterator end() const { return list.cend(); }

    reverse_iterator rbegin() { return list.rbegin(); }
    const_reverse_iterator rbegin() const { return list.crbegin(); }
    reverse_iterator rend() { return list.rend(); }
    const_reverse_iterator rend() const { return list.crend(); }
};


}

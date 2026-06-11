#ifndef LIST_H
#define LIST_H

#include "../cpu/types.h"
#include "../mm/heap.h"

namespace list {

    static const size_t DEFAULT_BLOCK_CAPACITY = 64;

    template<typename T>
    struct Block {
        size_t   used;
        Block<T>* next;
        T         data[];
    };

    template<typename T>
    struct List {
        size_t    size;
        size_t    block_capacity;
        Block<T>* first_block;
        Block<T>* last_block;
    };

    template<typename T>
    List<T>* create(size_t block_capacity = DEFAULT_BLOCK_CAPACITY) {
        List<T>* list = (List<T>*)kmalloc(sizeof(List<T>));
        if (!list) return nullptr;

        list->size = 0;
        list->block_capacity = block_capacity;
        list->first_block = nullptr;
        list->last_block = nullptr;
        return list;
    }

    template<typename T>
    static Block<T>* allocate_block(size_t capacity) {
        Block<T>* block = (Block<T>*)kmalloc(sizeof(Block<T>) + capacity * sizeof(T));
        if (!block) return nullptr;

        block->used = 0;
        block->next = nullptr;
        return block;
    }

    template<typename T>
    void destroy(List<T>* list) {
        if (!list) return;

        Block<T>* current = list->first_block;
        while (current) {
            Block<T>* next = current->next;
            kfree(current);
            current = next;
        }
        kfree(list);
    }

    template<typename T>
    bool add(List<T>* list, T value) {
        if (!list) return false;

        if (!list->last_block || list->last_block->used >= list->block_capacity) {
            Block<T>* new_block = allocate_block<T>(list->block_capacity);
            if (!new_block) return false;

            if (!list->first_block) {
                list->first_block = new_block;
                list->last_block = new_block;
            } else {
                list->last_block->next = new_block;
                list->last_block = new_block;
            }
        }

        list->last_block->data[list->last_block->used] = value;
        list->last_block->used++;
        list->size++;
        return true;
    }

    template<typename T>
    struct BlockPosition {
        Block<T>* block;
        size_t    local_index;
    };

    template<typename T>
    BlockPosition<T> find_position(List<T>* list, size_t global_index) {
        BlockPosition<T> pos = {nullptr, 0};
        if (!list || global_index >= list->size) return pos;

        Block<T>* current = list->first_block;
        size_t current_index = 0;

        while (current) {
            if (global_index < current_index + current->used) {
                pos.block = current;
                pos.local_index = global_index - current_index;
                break;
            }
            current_index += current->used;
            current = current->next;
        }
        return pos;
    }

    template<typename T>
    bool get(List<T>* list, size_t index, T& out) {
        BlockPosition<T> pos = find_position(list, index);
        if (!pos.block) return false;

        out = pos.block->data[pos.local_index];
        return true;
    }

    template<typename T>
    void set(List<T>* list, size_t index, T value) {
        BlockPosition<T> pos = find_position(list, index);
        if (!pos.block) return;

        pos.block->data[pos.local_index] = value;
    }

    template<typename T>
    void remove_at(List<T>* list, size_t index) {
        if (!list || index >= list->size) return;

        BlockPosition<T> pos = find_position(list, index);
        if (!pos.block) return;

        for (size_t i = pos.local_index; i < pos.block->used - 1; i++)
            pos.block->data[i] = pos.block->data[i + 1];
        pos.block->used--;

        if (pos.block->used == 0 && list->size > 1) {
            if (pos.block == list->first_block) {
                list->first_block = pos.block->next;
                if (list->last_block == pos.block)
                    list->last_block = list->first_block;
            } else {
                Block<T>* prev = list->first_block;
                while (prev && prev->next != pos.block)
                    prev = prev->next;
                if (prev) {
                    prev->next = pos.block->next;
                    if (list->last_block == pos.block)
                        list->last_block = prev;
                }
            }
            kfree(pos.block);
        }

        // Перебалансировка: подтягиваем элементы из следующих блоков
        Block<T>* current = pos.block->next;
        Block<T>* prev_block = pos.block;

        while (current && current->used > 0) {
            if (prev_block->used < list->block_capacity) {
                prev_block->data[prev_block->used] = current->data[0];
                prev_block->used++;

                for (size_t i = 0; i < current->used - 1; i++)
                    current->data[i] = current->data[i + 1];
                current->used--;
            }
            prev_block = current;
            current = current->next;
        }

        list->size--;
    }

    template<typename T>
    size_t size(List<T>* list) {
        return list ? list->size : 0;
    }

    template<typename T>
    void clear(List<T>* list) {
        if (!list) return;

        Block<T>* current = list->first_block;
        while (current) {
            Block<T>* next = current->next;
            kfree(current);
            current = next;
        }
        list->first_block = nullptr;
        list->last_block = nullptr;
        list->size = 0;
    }
}

#endif
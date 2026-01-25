#ifndef LIST_H
#define LIST_H
#include "../cpu/types.h"
#include "../mm/memory.h"

namespace list {
    template<typename T>
    struct Block {
        uint64_t used;           // Количество использованных элементов в блоке
        Block<T>* next;        // Указатель на следующий блок
        T data[];              // Массив данных (flexible array member)
    };

    template<typename T>
    struct List {
        uint64_t size;           // Общее количество элементов во всем списке
        uint64_t block_capacity; // Количество элементов в одном блоке
        Block<T>* first_block; // Указатель на первый блок
        Block<T>* last_block;  // Указатель на последний блок (для быстрого добавления)
    };

    template<typename T>
    List<T>* create() {
        List<T>* list = (List<T>*)memory::memalloc();
        if (!list) return nullptr;

        list->size = 0;
        list->block_capacity = (ALLOCATOR_BLOCK_SIZE - sizeof(Block<T>)) / sizeof(T);
        list->first_block = nullptr;
        list->last_block = nullptr;
        
        return list;
    }

    template<typename T>
    Block<T>* allocate_block() {
        Block<T>* block = (Block<T>*)memory::memalloc();
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
            memory::buddy_free((uint8_t*)current);
            current = next;
        }
        
        memory::buddy_free((uint8_t*)list);
    }

    template<typename T>
    bool add(List<T>* list, T value) {
        if (!list) return false;

        if (!list->last_block || list->last_block->used >= list->block_capacity) {
            Block<T>* new_block = allocate_block<T>();
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
        uint64_t local_index;
    };

    template<typename T>
    BlockPosition<T> find_position(List<T>* list, uint64_t global_index) {
        BlockPosition<T> pos = {nullptr, 0};
        if (!list || global_index >= list->size) return pos;
        
        Block<T>* current = list->first_block;
        uint64_t current_index = 0;
        
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
    bool get(List<T>* list, uint64_t index, T& out) {
        BlockPosition<T> pos = find_position(list, index);
        if (!pos.block) return false;
        
        out = pos.block->data[pos.local_index];
        return true;
    }

    template<typename T>
    void set(List<T>* list, uint64_t index, T value) {
        BlockPosition<T> pos = find_position(list, index);
        if (!pos.block) return;
        
        pos.block->data[pos.local_index] = value;
    }

    template<typename T>
    void remove_at(List<T>* list, uint64_t index) {
        if (!list || index >= list->size) return;
        
        BlockPosition<T> pos = find_position(list, index);
        if (!pos.block) return;
        
        // Сдвигаем элементы внутри текущего блока
        for (uint64_t i = pos.local_index; i < pos.block->used - 1; i++) {
            pos.block->data[i] = pos.block->data[i + 1];
        }
        pos.block->used--;
        
        // Если блок стал пустым и это не единственный блок, можем его удалить
        if (pos.block->used == 0 && list->size > 1) {
            // Нужно найти предыдущий блок для перелинковки
            if (pos.block == list->first_block) {
                list->first_block = pos.block->next;
                if (list->last_block == pos.block) {
                    list->last_block = list->first_block;
                }
            } else {
                Block<T>* prev = list->first_block;
                while (prev && prev->next != pos.block) {
                    prev = prev->next;
                }
                if (prev) {
                    prev->next = pos.block->next;
                    if (list->last_block == pos.block) {
                        list->last_block = prev;
                    }
                }
            }
            memory::memfree((uint8_t*)pos.block);
        }
        
        // Сдвигаем элементы из следующих блоков
        Block<T>* current = pos.block->next;
        Block<T>* prev_block = pos.block;
        
        while (current && current->used > 0) {
            // Перемещаем первый элемент текущего блока в конец предыдущего
            if (prev_block->used < list->block_capacity) {
                prev_block->data[prev_block->used] = current->data[0];
                prev_block->used++;
                
                // Сдвигаем элементы в текущем блоке
                for (uint64_t i = 0; i < current->used - 1; i++) {
                    current->data[i] = current->data[i + 1];
                }
                current->used--;
            }
            
            prev_block = current;
            current = current->next;
        }
        
        list->size--;
    }

    template<typename T>
    uint64_t size(List<T>* list) {
        return list ? list->size : 0;
    }

    template<typename T>
    void clear(List<T>* list) {
        if (!list) return;
        
        Block<T>* current = list->first_block;
        while (current) {
            Block<T>* next = current->next;
            memory::memfree((uint8_t*)current);
            current = next;
        }
        
        list->first_block = nullptr;
        list->last_block = nullptr;
        list->size = 0;
    }
} // namespace

#endif
#ifndef BFCEXT_SHARED_SIZED_BUFFER_HPP_
#define BFCEXT_SHARED_SIZED_BUFFER_HPP_

#include <bfc/buffer.hpp>

#include <cstddef>
#include <cstring>
#include <memory>

namespace bfcext
{

// Copyable view over shared storage with a separate logical size (like
// bfc::sized_buffer, but refcounted). Multiple views may reference the same
// bytes at different offsets.
class shared_sized_buffer
{
public:
    struct Storage
    {
        bfc::buffer bytes{};
    };

    shared_sized_buffer() = default;

    static shared_sized_buffer copy_from(const void* data, size_t len)
    {
        shared_sized_buffer out;
        if (data == nullptr || len == 0)
        {
            return out;
        }
        out.resize(len);
        std::memcpy(out.data(), data, len);
        return out;
    }

    size_t size() const noexcept
    {
        return size_;
    }

    size_t offset() const noexcept
    {
        return offset_;
    }

    size_t capacity() const noexcept
    {
        if (storage_ == nullptr)
        {
            return 0;
        }
        return storage_->bytes.size() - offset_;
    }

    bool empty() const noexcept
    {
        return storage_ == nullptr || size_ == 0;
    }

    std::byte* data() const noexcept
    {
        if (storage_ == nullptr)
        {
            return nullptr;
        }
        return storage_->bytes.data() + offset_;
    }

    void clear() noexcept
    {
        storage_.reset();
        offset_ = 0;
        size_ = 0;
    }

    void reserve(size_t new_capacity)
    {
        if (offset_ != 0)
        {
            return;
        }
        if (new_capacity <= capacity())
        {
            return;
        }

        auto next = std::make_shared<Storage>();
        next->bytes =
            bfc::buffer(reinterpret_cast<std::byte*>(new std::byte[new_capacity]),
                        new_capacity);
        if (storage_ != nullptr && size_ > 0)
        {
            std::memcpy(next->bytes.data(), data(), size_);
        }
        storage_ = std::move(next);
        offset_ = 0;
    }

    void resize(size_t new_size)
    {
        if (offset_ != 0)
        {
            return;
        }
        if (new_size > capacity())
        {
            reserve(new_size);
        }
        size_ = new_size;
    }

    bfc::const_buffer_view view() const noexcept
    {
        return bfc::const_buffer_view(data(), size());
    }

    shared_sized_buffer subview(size_t rel_offset, size_t len) const noexcept
    {
        shared_sized_buffer out;
        if (storage_ == nullptr || rel_offset > size_ ||
            len > size_ - rel_offset)
        {
            return out;
        }
        out.storage_ = storage_;
        out.offset_ = offset_ + rel_offset;
        out.size_ = len;
        return out;
    }

    std::shared_ptr<Storage> storage() const noexcept
    {
        return storage_;
    }

private:
    std::shared_ptr<Storage> storage_{};
    size_t offset_ = 0;
    size_t size_ = 0;
};

}  // namespace bfcext

#endif  // BFCEXT_SHARED_SIZED_BUFFER_HPP_

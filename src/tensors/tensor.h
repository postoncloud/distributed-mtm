#pragma once

#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>

#include "3rd_party/exception.h"
#include "common/definitions.h"
#include "common/shape.h"

namespace marian {

class TensorBase : public std::enable_shared_from_this<TensorBase> {
private:
  float* data_;
  Shape shape_;
  size_t device_;

public:
  TensorBase(float* data, Shape shape, size_t device)
      : data_(data), shape_(shape), device_(device) {}

  ~TensorBase() {}

  virtual void reset(float* data) { data_ = data; }

  virtual float* data() { return data_; }

  virtual Shape& shape() { return shape_; }

  virtual size_t size() { return shape_.elements(); }

  virtual float scalar() {
    UTIL_THROW_IF2(size() != 1, "Tensor is not a scalar");
    return get(0);
  }

  size_t getDevice() { return device_; }

  Tensor subtensor(int offset, int size) {
    return Tensor(new TensorBase(data_ + offset, {1, size}, device_));
  }

  float get(size_t i);

  void set(size_t i, float value);

  void get(std::vector<float>& v);

  void set(float value);

  void set(const std::vector<float>& v);

  void setSparse(const std::vector<size_t>& k, const std::vector<float>& v);

  void copyFrom(Tensor);

  std::string debug();
};

class DeviceGPU {
private:
  float* data_;
  size_t size_;
  size_t device_;

public:
  DeviceGPU(size_t device) : data_(0), size_(0), device_(device) {}

  ~DeviceGPU();

  typedef TensorBase tensor_type;

  void reserve(size_t size);

  float* data() { return data_; }

  size_t capacity() { return size_; }

  size_t getDevice() { return device_; }
};

typedef std::shared_ptr<TensorBase> Tensor;

Tensor operator<<(Tensor t, const std::vector<float>& v);

Tensor operator>>(Tensor t, std::vector<float>& v);
}

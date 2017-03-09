/*
 * Copyright (C) 2011 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ART_COMPILER_COMPILED_METHOD_H_
#define ART_COMPILER_COMPILED_METHOD_H_

#include <memory>
#include <string>
#include <vector>

#include "instruction_set.h"
#include "utils.h"
#include "utils/array_ref.h"
#include "utils/swap_space.h"

namespace llvm {
  class Function;
}  // namespace llvm

namespace art {

class CompilerDriver;

class CompiledCode {
 public:
  // For Quick to supply an code blob
  CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
               const ArrayRef<const uint8_t>& quick_code);

  // For Portable to supply an ELF object
  CompiledCode(CompilerDriver* compiler_driver, InstructionSet instruction_set,
               const std::string& elf_object, const std::string &symbol);

  InstructionSet GetInstructionSet() const {
    return instruction_set_;
  }

  const SwapVector<uint8_t>* GetPortableCode() const {
    return portable_code_;
  }

  const SwapVector<uint8_t>* GetQuickCode() const {
    return quick_code_;
  }

  void SetCode(const ArrayRef<const uint8_t>* quick_code,
               const ArrayRef<const uint8_t>* portable_code);

  bool operator==(const CompiledCode& rhs) const;

  // To align an offset from a page-aligned value to make it suitable
  // for code storage. For example on ARM, to ensure that PC relative
  // valu computations work out as expected.
  uint32_t AlignCode(uint32_t offset) const;
  static uint32_t AlignCode(uint32_t offset, InstructionSet instruction_set);

  // returns the difference between the code address and a usable PC.
  // mainly to cope with kThumb2 where the lower bit must be set.
  size_t CodeDelta() const;
  static size_t CodeDelta(InstructionSet instruction_set);

  // Returns a pointer suitable for invoking the code at the argument
  // code_pointer address.  Mainly to cope with kThumb2 where the
  // lower bit must be set to indicate Thumb mode.
  static const void* CodePointer(const void* code_pointer,
                                 InstructionSet instruction_set);

  const std::string& GetSymbol() const;
  const std::vector<uint32_t>& GetOatdataOffsetsToCompliledCodeOffset() const;
  void AddOatdataOffsetToCompliledCodeOffset(uint32_t offset);

 private:
  CompilerDriver* const compiler_driver_;

  const InstructionSet instruction_set_;

  // The ELF image for portable.
  SwapVector<uint8_t>* portable_code_;

  // Used to store the PIC code for Quick.
  SwapVector<uint8_t>* quick_code_;

  // Used for the Portable ELF symbol name.
  const std::string symbol_;

  // There are offsets from the oatdata symbol to where the offset to
  // the compiled method will be found. These are computed by the
  // OatWriter and then used by the ElfWriter to add relocations so
  // that MCLinker can update the values to the location in the linked .so.
  std::vector<uint32_t> oatdata_offsets_to_compiled_code_offset_;
};

class SrcMapElem {
 public:
  uint32_t from_;
  int32_t to_;

  explicit operator int64_t() const {
    return (static_cast<int64_t>(to_) << 32) | from_;
  }

  bool operator<(const SrcMapElem& sme) const {
    return int64_t(*this) < int64_t(sme);
  }

  bool operator==(const SrcMapElem& sme) const {
    return int64_t(*this) == int64_t(sme);
  }

  explicit operator uint8_t() const {
    return static_cast<uint8_t>(from_ + to_);
  }
};

template <class Allocator>
class SrcMap FINAL : public std::vector<SrcMapElem, Allocator> {
 public:
  using std::vector<SrcMapElem, Allocator>::begin;
  using typename std::vector<SrcMapElem, Allocator>::const_iterator;
  using std::vector<SrcMapElem, Allocator>::empty;
  using std::vector<SrcMapElem, Allocator>::end;
  using std::vector<SrcMapElem, Allocator>::resize;
  using std::vector<SrcMapElem, Allocator>::shrink_to_fit;
  using std::vector<SrcMapElem, Allocator>::size;

  explicit SrcMap() {}

  template <class InputIt>
  SrcMap(InputIt first, InputIt last, const Allocator& alloc)
      : std::vector<SrcMapElem, Allocator>(first, last, alloc) {}


  void SortByFrom() {
    std::sort(begin(), end(), [] (const SrcMapElem& lhs, const SrcMapElem& rhs) -> bool {
      return lhs.from_ < rhs.from_;
    });
  }

  const_iterator FindByTo(int32_t to) const {
    return std::lower_bound(begin(), end(), SrcMapElem({0, to}));
  }

  SrcMap& Arrange() {
    if (!empty()) {
      std::sort(begin(), end());
      resize(std::unique(begin(), end()) - begin());
      shrink_to_fit();
    }
    return *this;
  }

  void DeltaFormat(const SrcMapElem& start, uint32_t highest_pc) {
    // Convert from abs values to deltas.
    if (!empty()) {
      SortByFrom();

      // TODO: one PC can be mapped to several Java src lines.
      // do we want such a one-to-many correspondence?

      // get rid of the highest values
      size_t i = size() - 1;
      for (; i > 0 ; i--) {
        if ((*this)[i].from_ < highest_pc) {
          break;
        }
      }
      this->resize(i + 1);

      for (i = size(); --i >= 1; ) {
        (*this)[i].from_ -= (*this)[i-1].from_;
        (*this)[i].to_ -= (*this)[i-1].to_;
      }
      DCHECK((*this)[0].from_ >= start.from_);
      (*this)[0].from_ -= start.from_;
      (*this)[0].to_ -= start.to_;
    }
  }
};

using DefaultSrcMap = SrcMap<std::allocator<SrcMapElem>>;
using SwapSrcMap = SrcMap<SwapAllocator<SrcMapElem>>;

class CompiledMethod FINAL : public CompiledCode {
 public:
  // Constructs a CompiledMethod for the non-LLVM compilers.
  CompiledMethod(CompilerDriver* driver,
                 InstructionSet instruction_set,
                 const ArrayRef<const uint8_t>& quick_code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask,
                 DefaultSrcMap* src_mapping_table,
                 const ArrayRef<const uint8_t>& mapping_table,
                 const ArrayRef<const uint8_t>& vmap_table,
                 const ArrayRef<const uint8_t>& native_gc_map,
                 const ArrayRef<const uint8_t>& cfi_info);

  // Constructs a CompiledMethod for the QuickJniCompiler.
  CompiledMethod(CompilerDriver* driver,
                 InstructionSet instruction_set,
                 const ArrayRef<const uint8_t>& quick_code,
                 const size_t frame_size_in_bytes,
                 const uint32_t core_spill_mask,
                 const uint32_t fp_spill_mask,
                 const ArrayRef<const uint8_t>& cfi_info);

  // Constructs a CompiledMethod for the Portable compiler.
  CompiledMethod(CompilerDriver* driver, InstructionSet instruction_set, const std::string& code,
                 const ArrayRef<const uint8_t>& gc_map, const std::string& symbol);

  // Constructs a CompiledMethod for the Portable JniCompiler.
  CompiledMethod(CompilerDriver* driver, InstructionSet instruction_set, const std::string& code,
                 const std::string& symbol);

  ~CompiledMethod() {}

  static CompiledMethod* SwapAllocCompiledMethod(CompilerDriver* driver,
                                                 InstructionSet instruction_set,
                                                 const ArrayRef<const uint8_t>& quick_code,
                                                 const size_t frame_size_in_bytes,
                                                 const uint32_t core_spill_mask,
                                                 const uint32_t fp_spill_mask,
                                                 DefaultSrcMap* src_mapping_table,
                                                 const ArrayRef<const uint8_t>& mapping_table,
                                                 const ArrayRef<const uint8_t>& vmap_table,
                                                 const ArrayRef<const uint8_t>& native_gc_map,
                                                 const ArrayRef<const uint8_t>& cfi_info);

  static CompiledMethod* SwapAllocCompiledMethod(CompilerDriver* driver,
                                                 InstructionSet instruction_set,
                                                 const ArrayRef<const uint8_t>& quick_code,
                                                 const size_t frame_size_in_bytes,
                                                 const uint32_t core_spill_mask,
                                                 const uint32_t fp_spill_mask,
                                                 const ArrayRef<const uint8_t>& cfi_info);

  static void ReleaseSwapAllocatedCompiledMethod(CompilerDriver* driver, CompiledMethod* m);

  size_t GetFrameSizeInBytes() const {
    return frame_size_in_bytes_;
  }

  uint32_t GetCoreSpillMask() const {
    return core_spill_mask_;
  }

  uint32_t GetFpSpillMask() const {
    return fp_spill_mask_;
  }

  const SwapSrcMap& GetSrcMappingTable() const {
    DCHECK(src_mapping_table_ != nullptr);
    return *src_mapping_table_;
  }

  const SwapVector<uint8_t>& GetMappingTable() const {
    DCHECK(mapping_table_ != nullptr);
    return *mapping_table_;
  }

  const SwapVector<uint8_t>& GetVmapTable() const {
    DCHECK(vmap_table_ != nullptr);
    return *vmap_table_;
  }

  const SwapVector<uint8_t>& GetGcMap() const {
    DCHECK(gc_map_ != nullptr);
    return *gc_map_;
  }

  const SwapVector<uint8_t>* GetCFIInfo() const {
    return cfi_info_;
  }

 private:
  // For quick code, the size of the activation used by the code.
  const size_t frame_size_in_bytes_;
  // For quick code, a bit mask describing spilled GPR callee-save registers.
  const uint32_t core_spill_mask_;
  // For quick code, a bit mask describing spilled FPR callee-save registers.
  const uint32_t fp_spill_mask_;
  // For quick code, a set of pairs (PC, Line) mapping from native PC offset to Java line
  SwapSrcMap* src_mapping_table_;
  // For quick code, a uleb128 encoded map from native PC offset to dex PC aswell as dex PC to
  // native PC offset. Size prefixed.
  SwapVector<uint8_t>* mapping_table_;
  // For quick code, a uleb128 encoded map from GPR/FPR register to dex register. Size prefixed.
  SwapVector<uint8_t>* vmap_table_;
  // For quick code, a map keyed by native PC indices to bitmaps describing what dalvik registers
  // are live. For portable code, the key is a dalvik PC.
  SwapVector<uint8_t>* gc_map_;
  // For quick code, a FDE entry for the debug_frame section.
  SwapVector<uint8_t>* cfi_info_;
};

}  // namespace art

#endif  // ART_COMPILER_COMPILED_METHOD_H_

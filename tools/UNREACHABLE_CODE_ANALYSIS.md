# Analysis: Why LTO Cannot Eliminate Unreachable Code

## Summary
Total functions in binary: 298
Reachable from main(): 33
Unreachable: 265 (89%)

Despite LTO's aggressive optimization, 265 functions remain unreachable. Here's why:

## 1. Virtual Function Tables (Vtables) - Primary Cause

### Ref Storage Backends
**Problem**: Git supports multiple reference storage backends via a vtable array:

```c
// In refs.c
static const struct ref_storage_be *refs_backends[] = {
    [REF_STORAGE_FORMAT_FILES] = &refs_be_files,
    [REF_STORAGE_FORMAT_REFTABLE] = &refs_be_reftable,
};
```

**Impact**: Even though we only use the FILES backend, the reftable vtable is included, forcing the linker to keep ALL functions pointed to by that vtable.

**Unreachable reftable functions**: 37
- reftable_be_init, reftable_be_transaction_prepare
- reftable_be_pack_refs, reftable_be_reflog_expire
- reftable_ref_record_*, reftable_obj_record_*
- And 30+ more

**Why LTO can't eliminate them**: The address of `refs_be_reftable` is taken and stored in a global array. The linker cannot prove that these function pointers are never dereferenced at runtime.

### Files Backend Unused Functions
Even within the files backend vtable, we don't use all operations:
- files_create_reflog, files_delete_reflog
- files_reflog_expire, files_for_each_reflog_ent
- files_fsck, files_ref_store_create_on_disk

**Why kept**: All in the same vtable, addresses taken.

### Hash Algorithm Vtables
**Problem**: Similar vtable for hash algorithms:

```c
// In hash.c  
const struct git_hash_algo hash_algos[GIT_HASH_NALGOS] = {
    [GIT_HASH_UNKNOWN] = { .init_fn = git_hash_unknown_init, ... },
    [GIT_HASH_SHA1] = { .init_fn = git_hash_sha1_init, ... },
    [GIT_HASH_SHA256] = { .init_fn = git_hash_sha256_init, ... },
};
```

**Unreachable SHA-256 functions**: 5
- git_hash_sha256_init, git_hash_sha256_update
- git_hash_sha256_final, git_hash_sha256_clone

**Unreachable SHA-1 "unsafe" functions**: 9
- git_hash_sha1_init_unsafe, git_hash_sha1_update_unsafe
- And 7 more variants we don't use

## 2. Trace2 Tracing Functions

**Unreachable trace2 functions**: 7
- trace2_child_exit_fl, trace2_cmd_error_va_fl
- trace2_region_enter_printf_va_fl, etc.

**Why kept**: Likely compiled in for all builds regardless of usage, or linked from object files containing needed trace functions.

## 3. Date Parsing Functions

**Unreachable**: 8 date parsing functions
- date_am, date_pm, date_tea, date_noon
- date_midnight, date_yesterday, date_never, date_now

**Why kept**: Part of date.o which contains needed date functions. LTO with -ffunction-sections should eliminate these, but they might be in a dispatch table.

## 4. Function Pointer Comparison Functions

Many `*_cmp` and `*_compare` functions remain:
- oidmap_neq, void_hashcmp, sha1_compare
- cmp_strmap_entry, namemap_cmp, pack_order_cmp

**Why kept**: Used as qsort/bsearch comparators or in hashmap vtables.

## 5. Iterator Vtables

Multiple iterator types with unused implementations:
- empty_ref_iterator_* (4 functions)
- prefix_ref_iterator_* (4 functions)  
- merge_ref_iterator_* (4 functions)

**Why kept**: Iterator vtables for different ref iteration strategies.

## Quantifying the Waste

| Category | Unreachable Functions | Estimate |
|----------|----------------------|----------|
| Reftable backend | 37 | ~150 KB |
| Hash algorithms | 14 | ~30 KB |
| Trace2 functions | 7 | ~20 KB |
| Date parsing | 8 | ~10 KB |
| Iterator vtables | 12 | ~15 KB |
| Files backend unused | 15 | ~40 KB |
| Other | 172 | ~150 KB |
| **Total** | **265** | **~415 KB** |

## Why This Happens with LTO

LTO is excellent at eliminating dead code, but it has fundamental limitations:

1. **Function pointers defeat static analysis**: When `&function` is taken, the linker must assume it could be called
2. **Vtables are inherently opaque**: Array indexing makes it impossible to prove certain slots are unused
3. **Conservative linking**: Better to include unused code than break runtime behavior

## Potential Solutions (for future work)

1. **Conditional compilation**: Use `#ifdef` to exclude entire backends at compile time
2. **Manual vtable pruning**: Patch git to remove unused backends from the array
3. **Link-time vtable specialization**: Advanced LTO could specialize vtables, but gcc doesn't support this for C
4. **Static analysis tools**: Use tools like `bloaty` to identify largest unreachable functions

## Conclusion

The remaining 265 unreachable functions (415KB) are mostly due to:
- **Vtable polymorphism** (ref backends, hash algos, iterators)
- **Conservative linking** (trace2, comparison functions)

This is expected behavior with C vtables. Further reduction would require modifying Git's source code to conditionally exclude backends, which is beyond the scope of build optimization.

The good news: We've already achieved 93% total size reduction (11MB → 769KB). The remaining 415KB of unreachable code represents the fundamental limit of LTO with C polymorphism.

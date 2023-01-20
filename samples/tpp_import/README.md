Configure IREE with:

```
-DCMAKE_C_FLAGS=-DIREE_HAL_EXECUTABLE_IMPORT_PROVIDER_DEFAULT_FN=iree_samples_tpp_import_provider
-DIREE_HAL_EXECUTABLE_LOADER_EXTRA_DEPS=iree_samples_tpp_import
```

package=crate_crossbeam-utils_z
$(package)_crate_name=crossbeam-utils
$(package)_version=0.8.8
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=0bf124c720b7686e3c2663cf54062ab0f68a88af2fb6a030e87e30bf721fcb38
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

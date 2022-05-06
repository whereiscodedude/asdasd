package=crate_crossbeam-epoch_z
$(package)_crate_name=crossbeam-epoch
$(package)_version=0.9.8
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=1145cf131a2c6ba0615079ab6a638f7e1973ac9c2634fcbeaaad6114246efe8c
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

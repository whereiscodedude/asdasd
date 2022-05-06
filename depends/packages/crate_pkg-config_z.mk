package=crate_pkg-config_z
$(package)_crate_name=pkg-config
$(package)_version=0.3.25
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=1df8c4ec4b0627e53bdf214615ad287367e482558cf84b109250b37464dc03ae
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

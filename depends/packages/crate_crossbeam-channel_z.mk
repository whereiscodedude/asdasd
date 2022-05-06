package=crate_crossbeam-channel_z
$(package)_crate_name=crossbeam-channel
$(package)_version=0.5.4
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=5aaa7bd5fb665c6864b5f963dd9097905c54125909c7aa94c9e18507cdbe6c53
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

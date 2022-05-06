package=crate_unicode-xid_z
$(package)_crate_name=unicode-xid
$(package)_version=0.2.3
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=957e51f3646910546462e67d5f7599b9e4fb8acdd304b087a6494730f9eebf04
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

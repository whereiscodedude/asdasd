package=crate_crc32fast_z
$(package)_crate_name=crc32fast
$(package)_version=1.3.2
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=b540bd8bc810d3885c6ea91e2018302f68baba2129ab3e88f32389ee9370880d
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

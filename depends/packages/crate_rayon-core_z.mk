package=crate_rayon-core_z
$(package)_crate_name=rayon-core
$(package)_version=1.9.2
$(package)_download_path=https://static.crates.io/crates/$($(package)_crate_name)
$(package)_file_name=$($(package)_crate_name)-$($(package)_version).crate
$(package)_sha256_hash=9f51245e1e62e1f1629cbfec37b5793bbabcaeb90f30e94d2ba03564687353e4
$(package)_crate_versioned_name=$($(package)_crate_name)

define $(package)_preprocess_cmds
  $(call generate_crate_checksum,$(package))
endef

define $(package)_stage_cmds
  $(call vendor_crate_source,$(package))
endef

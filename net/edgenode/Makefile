include $(TOPDIR)/rules.mk

PKG_NAME:=edgenode
PKG_VERSION:=0.3.17
PKG_RELEASE:=4

NANOPB_VERSION:=0.4.9.1
PKG_SOURCE:=nanopb-$(NANOPB_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/nanopb/nanopb/tar.gz/refs/tags/$(NANOPB_VERSION)?
PKG_HASH:=4575944a468718ef25f05eb01d994364650b581563089a9841986bb1e460eac3
PKG_BUILD_DIR:=$(BUILD_DIR)/nanopb-$(NANOPB_VERSION)
PKG_LICENSE:=Zlib MIT
PKG_LICENSE_FILES:=LICENSE
PKG_BUILD_FLAGS:=lto

include $(INCLUDE_DIR)/package.mk

define Package/edgenode
  SECTION:=net
  CATEGORY:=Network
  TITLE:=Small multi-platform IoT edge node
  URL:=https://github.com/hyird/iot-engine
  DEPENDS:=+libev +libuci +libuwsc-mbedtls +libmbedtls +uclient-fetch +libustream-mbedtls +ca-bundle
endef

define Package/edgenode/description
 A small OpenWrt edge daemon using HTTP/HTTPS WebSocket upgrades and nanopb. The node is
 identified by IMEI and writes raw nanopb config/outbox messages to tmpfs without a database.
endef

define Package/edgenode/conffiles
/etc/config/edgenode
endef

define Build/Prepare
	$(call Build/Prepare/Default)
	$(INSTALL_DIR) $(PKG_BUILD_DIR)/edge/src $(PKG_BUILD_DIR)/edge/proto \
		$(PKG_BUILD_DIR)/edge/generated
	$(CP) $(CURDIR)/src/* $(PKG_BUILD_DIR)/edge/src/
	$(CP) $(CURDIR)/proto/* $(PKG_BUILD_DIR)/edge/proto/
	$(CP) $(CURDIR)/generated/* $(PKG_BUILD_DIR)/edge/generated/
endef

define Build/Compile
	$(TARGET_CC) $(TARGET_CFLAGS) -std=gnu11 -Os -flto -D_FILE_OFFSET_BITS=64 \
		-Wall -Wextra -Werror -Wno-strict-aliasing \
		-ffunction-sections -fdata-sections -fno-unwind-tables \
		-I$(PKG_BUILD_DIR) -I$(PKG_BUILD_DIR)/edge/src \
		-I$(PKG_BUILD_DIR)/edge/generated \
		$(PKG_BUILD_DIR)/pb_common.c \
		$(PKG_BUILD_DIR)/pb_encode.c \
		$(PKG_BUILD_DIR)/pb_decode.c \
		$(PKG_BUILD_DIR)/edge/generated/edge.pb.c \
		$(PKG_BUILD_DIR)/edge/src/edge_protocol.c \
		$(PKG_BUILD_DIR)/edge/src/edge_retry.c \
		$(PKG_BUILD_DIR)/edge/src/edge_config.c \
		$(PKG_BUILD_DIR)/edge/src/edge_memory.c \
		$(PKG_BUILD_DIR)/edge/src/edge_outbox.c \
		$(PKG_BUILD_DIR)/edge/src/log.c \
		$(PKG_BUILD_DIR)/edge/src/edge_spool.c \
		$(PKG_BUILD_DIR)/edge/src/edge_runtime_config.c \
		$(PKG_BUILD_DIR)/edge/src/edge_process.c \
		$(PKG_BUILD_DIR)/edge/src/edge_interface.c \
		$(PKG_BUILD_DIR)/edge/src/edge_capability.c \
		$(PKG_BUILD_DIR)/edge/src/edge_network.c \
		$(PKG_BUILD_DIR)/edge/src/edge_firmware.c \
		$(PKG_BUILD_DIR)/edge/src/edge_terminal.c \
		$(PKG_BUILD_DIR)/edge/src/edge_ws.c \
		$(PKG_BUILD_DIR)/edge/src/edge_acquisition.c \
		$(PKG_BUILD_DIR)/edge/src/edge_device_runtime.c \
		$(PKG_BUILD_DIR)/edge/src/edge_modbus.c \
		$(PKG_BUILD_DIR)/edge/src/edge_s7.c \
		$(PKG_BUILD_DIR)/edge/src/edge_modem.c \
		$(PKG_BUILD_DIR)/edge/src/main.c \
		$(TARGET_LDFLAGS) -Wl,--gc-sections -flto \
		-lev -luwsc -luci -lmbedcrypto -lm \
		-o $(PKG_BUILD_DIR)/edgenode
endef

define Package/edgenode/install
	$(INSTALL_DIR) $(1)/usr/sbin
	$(INSTALL_BIN) $(PKG_BUILD_DIR)/edgenode $(1)/usr/sbin/edgenode
	$(INSTALL_BIN) $(CURDIR)/files/edgenode-start $(1)/usr/sbin/edgenode-start
	$(INSTALL_DIR) $(1)/etc/init.d
	$(INSTALL_BIN) $(CURDIR)/files/edgenode.init $(1)/etc/init.d/edgenode
	$(INSTALL_DIR) $(1)/etc/config
	$(INSTALL_CONF) $(CURDIR)/files/edgenode.config $(1)/etc/config/edgenode
	$(INSTALL_DIR) $(1)/etc/uci-defaults
	$(INSTALL_BIN) $(CURDIR)/files/95-edgenode-platform $(1)/etc/uci-defaults/95-edgenode-platform
	$(INSTALL_DIR) $(1)/usr/share/edgenode
	$(INSTALL_DATA) $(CURDIR)/files/NOTICE $(1)/usr/share/edgenode/NOTICE
endef

$(eval $(call BuildPackage,edgenode))

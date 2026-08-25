#
# Copyright (C) 2016 Shuanglei Tao <tsl0922@gmail.com>
#
# This is free software, licensed under the GNU General Public License v2.
# See /LICENSE for more information.
#

include $(TOPDIR)/rules.mk

PKG_NAME:=ttyd
PKG_VERSION:=1.7.7
PKG_RELEASE:=2

PKG_SOURCE:=$(PKG_NAME)-$(PKG_VERSION).tar.gz
PKG_SOURCE_URL:=https://codeload.github.com/tsl0922/ttyd/tar.gz/$(PKG_VERSION)?
PKG_HASH:=039dd995229377caee919898b7bd54484accec3bba49c118e2d5cd6ec51e3650

PKG_MAINTAINER:=Shuanglei Tao <tsl0922@gmail.com>
PKG_LICENSE:=MIT
PKG_LICENSE_FILES:=LICENSE
PKG_CPE_ID:=cpe:/a:ttyd_project:ttyd

include $(INCLUDE_DIR)/package.mk
include $(INCLUDE_DIR)/cmake.mk

define Package/ttyd/Default
	SECTION:=utils
	CATEGORY:=Utilities
	TITLE:=Command-line tool for sharing terminal over the web
	URL:=https://github.com/tsl0922/ttyd
	SUBMENU:=Terminal
endef

define Package/ttyd
	$(call Package/ttyd/Default)
	DEPENDS:=+libcap +libopenssl +libjson-c +libuv +zlib +libwebsockets-full
endef

define Package/ttyd-notls
	$(call Package/ttyd/Default)
	TITLE += (without TLS)
	DEPENDS:=+libcap +libjson-c +libuv +zlib +libwebsockets-notls
	PROVIDES:=ttyd
	CONFLICTS:=ttyd
endef

define Package/ttyd/description
ttyd is a command-line tool for sharing terminal over the web.
endef

define Package/ttyd/conffiles
/etc/config/ttyd
endef

Package/ttyd-notls/conffiles = $(Package/ttyd/conffiles)

define Package/ttyd/install
	$(INSTALL_DIR) $(1)/usr/bin
	$(INSTALL_BIN) $(PKG_INSTALL_DIR)/usr/bin/ttyd $(1)/usr/bin/
	$(INSTALL_DIR) $(1)/etc/config/
	$(INSTALL_CONF) ./files/ttyd.config $(1)/etc/config/ttyd
	$(INSTALL_DIR) $(1)/etc/init.d/
	$(INSTALL_BIN) ./files/ttyd.init $(1)/etc/init.d/ttyd
endef

Package/ttyd-notls/install = $(Package/ttyd/install)

$(eval $(call BuildPackage,ttyd))
$(eval $(call BuildPackage,ttyd-notls))

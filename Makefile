# Two independent release artifacts:
#   make patches [-> out/nimbus-patches.zip]   the .ips patches + nimbus.3gx + juxt-prod.pem   (tag patches-v*)
#   make app     [-> out/nimbus.cia]           the updater app, CIA only                        (tag app-v*)
#   make APP_VERSION=2.3.1 app                 stamps the app version (normally taken from the app-v tag)
.PHONY: all patches app clean

OUT_FOLDER      := out



NIMBUS_UPDATE_OUT   := 3ds/nimbus/update

FRIENDS_TITLE_ID    := 0004013000003202
ACT_TITLE_ID        := 0004013000003802
HTTP_TITLE_ID       := 0004013000002902
SOCKET_TITLE_ID     := 0004013000002E02
SSL_TITLE_ID        := 0004013000002F02
NIM_TITLE_ID        := 0004013000002C02
MINT_TITLE_ID_EUR   := 000400300000CE02
MINT_TITLE_ID_USA   := 000400300000D602
MINT_TITLE_ID_JPN   := 000400300000C602
MIIVERSE_ID_JPN     := 000400300000BC02
MIIVERSE_ID_USA     := 000400300000BD02
MIIVERSE_ID_EUR     := 000400300000BE02

ACT_OUT             := $(NIMBUS_UPDATE_OUT)/$(ACT_TITLE_ID).ips
FRIENDS_OUT         := $(NIMBUS_UPDATE_OUT)/$(FRIENDS_TITLE_ID).ips
HTTP_OUT            := $(NIMBUS_UPDATE_OUT)/$(HTTP_TITLE_ID).ips
SOCKET_OUT          := $(NIMBUS_UPDATE_OUT)/$(SOCKET_TITLE_ID).ips
SSL_OUT             := $(NIMBUS_UPDATE_OUT)/$(SSL_TITLE_ID).ips
NIM_OUT             := $(NIMBUS_UPDATE_OUT)/$(NIM_TITLE_ID).ips
MINT_OUT_EUR        := $(NIMBUS_UPDATE_OUT)/$(MINT_TITLE_ID_EUR).ips
MINT_OUT_USA        := $(NIMBUS_UPDATE_OUT)/$(MINT_TITLE_ID_USA).ips
MINT_OUT_JPN        := $(NIMBUS_UPDATE_OUT)/$(MINT_TITLE_ID_JPN).ips
MIIVERSE_OUT_JPN    := $(NIMBUS_UPDATE_OUT)/$(MIIVERSE_ID_JPN).ips
MIIVERSE_OUT_USA    := $(NIMBUS_UPDATE_OUT)/$(MIIVERSE_ID_USA).ips
MIIVERSE_OUT_EUR    := $(NIMBUS_UPDATE_OUT)/$(MIIVERSE_ID_EUR).ips
PLUGIN_OUT          := $(NIMBUS_UPDATE_OUT)/nimbus.3gx

PKG := $(OUT_FOLDER)/patches_pkg

ifneq ($(strip $(APP_VERSION)),)
APP_VERSION_ARGS := VERSION_MAJOR=$(word 1,$(subst ., ,$(APP_VERSION))) VERSION_MINOR=$(word 2,$(subst ., ,$(APP_VERSION))) VERSION_MICRO=$(word 3,$(subst ., ,$(APP_VERSION)))
endif

all: patches app

patches:
	@rm -rf $(PKG) $(OUT_FOLDER)/nimbus-patches.zip
	@mkdir -p $(PKG)/$(NIMBUS_UPDATE_OUT)

# build patches (needs the module dumps as patches/*/code.bin, see DECOMPRESSING.md)
	@$(MAKE) -C patches

	@cp -r patches/act/out/* $(PKG)/$(ACT_OUT)
	@cp -r patches/friends/out/* $(PKG)/$(FRIENDS_OUT)
	@cp -r patches/http/out/* $(PKG)/$(HTTP_OUT)
	@cp -r patches/socket/out/* $(PKG)/$(SOCKET_OUT)
	@cp -r patches/ssl/out/* $(PKG)/$(SSL_OUT)
	@cp -r patches/nim/out/* $(PKG)/$(NIM_OUT)
	@cp -r patches/mint/out/* $(PKG)/$(MINT_OUT_EUR)
	@cp -r patches/mint/out/* $(PKG)/$(MINT_OUT_USA)
	@cp -r patches/mint/out/* $(PKG)/$(MINT_OUT_JPN)
	@cp -r patches/miiverse/out/* $(PKG)/$(MIIVERSE_OUT_JPN)
	@cp -r patches/miiverse/out/* $(PKG)/$(MIIVERSE_OUT_USA)
	@cp -r patches/miiverse/out/* $(PKG)/$(MIIVERSE_OUT_EUR)
	@cp -r patches/miiverse/*.pem $(PKG)/$(NIMBUS_UPDATE_OUT)

# build plugin
	@$(MAKE) -C plugin
	@cp -r plugin/plugin.3gx $(PKG)/$(PLUGIN_OUT)

# package: the app reads files from 3ds/nimbus/update/ inside this zip
	@cd $(PKG) && zip -qr ../nimbus-patches.zip 3ds
	@rm -rf $(PKG)
	@echo built $(OUT_FOLDER)/nimbus-patches.zip

app:
	@mkdir -p $(OUT_FOLDER)
	@$(MAKE) -C app cia $(APP_VERSION_ARGS)
	@cp app/nimbus.cia $(OUT_FOLDER)/nimbus.cia
	@echo built $(OUT_FOLDER)/nimbus.cia

clean:
	@$(MAKE) -C patches clean
	@$(MAKE) -C plugin clean
	@$(MAKE) -C app clean
	@rm -rf $(OUT_FOLDER)

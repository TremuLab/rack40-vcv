ifndef RACK_DIR
$(error Set RACK_DIR to your official Rack SDK directory)
endif
SOURCES += src/plugin.cpp
DISTRIBUTABLES += res manual LICENSE.txt README.md CHANGELOG.md THIRD_PARTY_NOTICES.md
include $(RACK_DIR)/plugin.mk

CXX := g++
CXXFLAGS := -O2 -std=c++17 -D_FILE_OFFSET_BITS=64 -DFUSE_USE_VERSION=35
LDFLAGS := -lfuse3 -pthread

TARGET := kubsh

SOURCES := main.cpp vfs.cpp

PACKAGE_NAME := $(TARGET)
VERSION := 1.0
ARCH := amd64
DEB_FILENAME := kubsh.deb

BUILD_DIR := deb_build
INSTALL_DIR := $(BUILD_DIR)/usr/local/bin

DOCKER_IMAGE := kubsh-local
TEST_CONTAINER := kubsh-test-$(shell date +%s)

.PHONY: all clean deb run

all: $(TARGET)

$(TARGET): $(SOURCES)
	$(CXX) $(CXXFLAGS) -o $@ $(SOURCES) $(LDFLAGS)

deb: $(TARGET) | $(BUILD_DIR) $(INSTALL_DIR)
	cp $(TARGET) $(INSTALL_DIR)/

	mkdir -p $(BUILD_DIR)/DEBIAN

	@echo "Package: $(PACKAGE_NAME)" > $(BUILD_DIR)/DEBIAN/control
	@echo "Version: $(VERSION)" >> $(BUILD_DIR)/DEBIAN/control
	@echo "Architecture: $(ARCH)" >> $(BUILD_DIR)/DEBIAN/control
	@echo "Maintainer: $(USER)" >> $(BUILD_DIR)/DEBIAN/control
	@echo "Description: Simple shell with VFS using FUSE3" >> $(BUILD_DIR)/DEBIAN/control
	@echo "Depends: fuse3" >> $(BUILD_DIR)/DEBIAN/control

	dpkg-deb --build $(BUILD_DIR) $(DEB_FILENAME)

$(BUILD_DIR) $(INSTALL_DIR):
	mkdir -p $@

clean:
	rm -rf $(TARGET) $(BUILD_DIR) $(DEB_FILENAME)

run: $(TARGET)
	./$(TARGET)

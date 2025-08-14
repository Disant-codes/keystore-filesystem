CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread
LDFLAGS = -pthread

# Directories
SRC_DIR = src
DAEMON_DIR = $(SRC_DIR)/daemon
BUILD_DIR = build
INSTALL_DIR = /usr/local
SERVICE_DIR = /etc/systemd/system

# Source files
DAEMON_SRC = $(DAEMON_DIR)/keystored.c
DAEMON_HEADER = $(DAEMON_DIR)/keystored.h

# Object files
DAEMON_OBJ = $(BUILD_DIR)/keystored.o

# Executables
DAEMON_EXE = $(BUILD_DIR)/keystored

# Service file
SERVICE_FILE = keystored.service

# Default target
all: $(BUILD_DIR) $(DAEMON_EXE)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile daemon
$(DAEMON_EXE): $(DAEMON_OBJ)
	$(CC) $(DAEMON_OBJ) -o $@ $(LDFLAGS)

$(DAEMON_OBJ): $(DAEMON_SRC) $(DAEMON_HEADER) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

# Clean build files
clean:
	rm -rf $(BUILD_DIR)

# Install daemon binary
install-bin: $(DAEMON_EXE)
	@echo "Installing keystored binary..."
	sudo install -d $(INSTALL_DIR)/bin
	sudo install -m 755 $(DAEMON_EXE) $(INSTALL_DIR)/bin/
	@echo "Binary installed to $(INSTALL_DIR)/bin/keystored"

# Create system user and group
install-user:
	@echo "Creating keystored user and group..."
	@if ! getent group keystored > /dev/null 2>&1; then \
		sudo groupadd -r keystored; \
	fi
	@if ! getent passwd keystored > /dev/null 2>&1; then \
		sudo useradd -r -g keystored -s /bin/false -d /var/lib/keystored keystored; \
	fi
	@echo "User and group created"

# Install service file
install-service: $(SERVICE_FILE)
	@echo "Installing systemd service..."
	sudo install -m 644 $(SERVICE_FILE) $(SERVICE_DIR)/
	sudo systemctl daemon-reload
	@echo "Service installed to $(SERVICE_DIR)/$(SERVICE_FILE)"

# Full installation
install: install-user install-bin install-service
	@echo "Installation complete!"
	@echo "To start the service: sudo systemctl start keystored"
	@echo "To enable auto-start: sudo systemctl enable keystored"

# Uninstall everything
uninstall:
	@echo "Stopping and disabling service..."
	-sudo systemctl stop keystored
	-sudo systemctl disable keystored
	@echo "Removing service file..."
	-sudo rm -f $(SERVICE_DIR)/$(SERVICE_FILE)
	-sudo systemctl daemon-reload
	@echo "Removing binary..."
	-sudo rm -f $(INSTALL_DIR)/bin/keystored
	@echo "Removing user and group..."
	-sudo userdel keystored 2>/dev/null || true
	-sudo groupdel keystored 2>/dev/null || true
	@echo "Uninstallation complete"

# Run daemon in foreground (for testing)
run: $(DAEMON_EXE)
	$(DAEMON_EXE)

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: clean all

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean all

# Show help
help:
	@echo "Available targets:"
	@echo "  all          - Build daemon"
	@echo "  clean        - Remove build files"
	@echo "  install      - Full installation (user, binary, service)"
	@echo "  install-bin  - Install binary only"
	@echo "  install-user - Create system user/group"
	@echo "  install-service - Install systemd service"
	@echo "  uninstall    - Remove everything"
	@echo ""

.PHONY: all clean install install-bin install-user install-service uninstall debug release help

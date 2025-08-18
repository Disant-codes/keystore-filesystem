CC = gcc
CFLAGS = -Wall -Wextra -std=c99 -pthread -Iinclude
LDFLAGS = -pthread

# Directories
SRC_DIR = src
DAEMON_DIR = $(SRC_DIR)/daemon
CLIENT_DIR = $(SRC_DIR)/client
JOBS_DIR = $(SRC_DIR)/jobs
BUILD_DIR = build
INSTALL_DIR = /usr/local
SERVICE_DIR = /etc/systemd/system

# Source files
DAEMON_SRC = $(DAEMON_DIR)/keystored.c
DAEMON_HEADER = $(DAEMON_DIR)/keystored.h
CLIENT_SRC = $(CLIENT_DIR)/client.c
JOBS_SRC = $(JOBS_DIR)/job_executor.c

# Header files
JOBS_HEADER = include/job_executor.h

# Object files
DAEMON_OBJ = $(BUILD_DIR)/keystored.o
CLIENT_OBJ = $(BUILD_DIR)/client.o
JOBS_OBJ = $(BUILD_DIR)/job_executor.o

# Executables
DAEMON_EXE = $(BUILD_DIR)/keystored
CLIENT_EXE = $(BUILD_DIR)/client

# Service file
SERVICE_FILE = keystored.service

# Default target
all: $(BUILD_DIR) $(DAEMON_EXE) $(CLIENT_EXE)

# Create build directory
$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

# Compile daemon
$(DAEMON_EXE): $(DAEMON_OBJ) $(JOBS_OBJ)
	$(CC) $(DAEMON_OBJ) $(JOBS_OBJ) -o $@ $(LDFLAGS)

# Compile client
$(CLIENT_EXE): $(CLIENT_OBJ) $(JOBS_OBJ)
	$(CC) $(CLIENT_OBJ) $(JOBS_OBJ) -o $@ $(LDFLAGS)

# Compile object files
$(DAEMON_OBJ): $(DAEMON_SRC) $(DAEMON_HEADER) $(JOBS_HEADER) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(CLIENT_OBJ): $(CLIENT_SRC) $(JOBS_HEADER) | $(BUILD_DIR)
	$(CC) $(CFLAGS) -c $< -o $@

$(JOBS_OBJ): $(JOBS_SRC) $(JOBS_HEADER) | $(BUILD_DIR)
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

# Install client binary
install-client: $(CLIENT_EXE)
	@echo "Installing client binary..."
	sudo install -d $(INSTALL_DIR)/bin
	sudo install -m 755 $(CLIENT_EXE) $(INSTALL_DIR)/bin/
	@echo "Client installed to $(INSTALL_DIR)/bin/client"

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
install: install-user install-bin install-client install-service
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
	@echo "Removing binaries..."
	-sudo rm -f $(INSTALL_DIR)/bin/keystored
	-sudo rm -f $(INSTALL_DIR)/bin/client
	@echo "Removing user and group..."
	-sudo userdel keystored 2>/dev/null || true
	-sudo groupdel keystored 2>/dev/null || true
	@echo "Uninstallation complete"

# Run daemon in foreground (for testing)
run: $(DAEMON_EXE)
	$(DAEMON_EXE)

# Run client in foreground (for testing)
run-client: $(CLIENT_EXE)
	$(CLIENT_EXE)

# Debug build
debug: CFLAGS += -g -DDEBUG
debug: clean all

# Release build
release: CFLAGS += -O2 -DNDEBUG
release: clean all

# Show help
help:
	@echo "Available targets:"
	@echo "  all          - Build daemon and client"
	@echo "  clean        - Remove build files"
	@echo "  install      - Full installation (user, binaries, service)"
	@echo "  install-bin  - Install daemon binary only"
	@echo "  install-client - Install client binary only"
	@echo "  install-user - Create system user/group"
	@echo "  install-service - Install systemd service"
	@echo "  uninstall    - Remove everything"
	@echo "  run          - Run daemon in foreground"
	@echo "  run-client   - Run client in foreground"
	@echo "  debug        - Build with debug flags"
	@echo "  release      - Build with release flags"
	@echo ""

.PHONY: all clean install install-bin install-client install-user install-service uninstall debug release help run run-client

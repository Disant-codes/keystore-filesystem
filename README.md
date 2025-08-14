# Key-Value Storage Daemon

A high-performance TCP-based key-value storage daemon that uses epoll for non-blocking I/O and processes client requests through an asynchronous job queue.

## Features

- **Daemon Process**: Runs as a background service
- **Signal Handling**: Graceful shutdown on SIGTERM/SIGINT
- **Comprehensive Logging**: Detailed logging via syslog/journald

## Prerequisites

- GCC compiler 
- systemd 

## Building

```bash
# Build the daemon
make all

# Build with debug symbols
make debug

# Build optimized release version
make release
```

## Installation

### Quick Installation

```bash
# Build and install everything
make all
sudo make install

# Enable and start the service
sudo make enable
sudo make start
```

### Step-by-Step Installation

1. **Build the daemon:**
   ```bash
   make all
   ```

2. **Create system user and group:**
   ```bash
   sudo make install-user
   ```

3. **Install the binary:**
   ```bash
   sudo make install-bin
   ```

4. **Install systemd service:**
   ```bash
   sudo make install-service
   ```

5. **Start the service:**
   ```bash
   sudo make start
   ```

6. **Enable auto-start on boot:**
   ```bash
   sudo make enable
   ```

## Usage
You can also use systemctl directly:

```bash
# Start service
sudo systemctl start keystored

# Stop service
sudo systemctl stop keystored

# Check status
sudo systemctl status keystored

# View logs
sudo journalctl -u keystored -f

# Enable on boot
sudo systemctl enable keystored
```

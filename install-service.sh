#!/bin/bash

# MachineMonitor systemd service installer

set -e

APP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SERVICE_NAME="machinemonitor"
SERVICE_FILE="/etc/systemd/system/${SERVICE_NAME}.service"

echo "Installing ${SERVICE_NAME} as a systemd service..."

# Create systemd service file
sudo tee "$SERVICE_FILE" > /dev/null <<EOF
[Unit]
Description=MachineMonitor - Real-time system metrics dashboard
After=network.target

[Service]
Type=simple
User=$USER
WorkingDirectory=$APP_DIR
Environment="PATH=$APP_DIR/venv/bin:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin"
ExecStart=$APP_DIR/venv/bin/python3 -m uvicorn app:app --host 0.0.0.0 --port 7777
Restart=always
RestartSec=10
StandardOutput=journal
StandardError=journal

[Install]
WantedBy=multi-user.target
EOF

echo "✓ Service file created at $SERVICE_FILE"

# Reload systemd daemon
echo "Reloading systemd daemon..."
sudo systemctl daemon-reload
echo "✓ Systemd daemon reloaded"

# Enable the service to start on boot
echo "Enabling service to start on boot..."
sudo systemctl enable "$SERVICE_NAME"
echo "✓ Service enabled"

echo ""
echo "Installation complete! Use these commands to manage the service:"
echo "  sudo systemctl start $SERVICE_NAME       # Start the service"
echo "  sudo systemctl stop $SERVICE_NAME        # Stop the service"
echo "  sudo systemctl restart $SERVICE_NAME     # Restart the service"
echo "  sudo systemctl status $SERVICE_NAME      # Check service status"
echo "  sudo journalctl -u $SERVICE_NAME -f      # View live logs"
echo ""
echo "Dashboard will be available at: http://localhost:7777"

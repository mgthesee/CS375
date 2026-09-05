#!/bin/bash

# Define the log file
LOGFILE="advanced-log.txt"

# Add a timestamp header
echo "=== Directory Listing: $(date) ===" > "$LOGFILE"

# List directory contents and append to the log file
# Using 'ls -la' gives detailed file info and includes hidden files
tree >> "$LOGFILE"

echo "Directory listing saved to $LOGFILE"

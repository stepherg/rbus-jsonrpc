# rbus-jsonrpc

`rbus-jsonrpc` is a JSON-RPC server implementation for interacting with the RDK Bus (rbus) over WebSocket. It provides a bridge between rbus and JSON-RPC, allowing clients to perform rbus operations (get, set, and event subscriptions) using a standard JSON-RPC 2.0 interface.

## Features

- **JSON-RPC over WebSocket**: Communicate with an rbus provider using WebSocket connections.
- **Supported Methods**:
  - `rbus_get`: Retrieve values for one or more rbus data model paths.
  - `rbus_set`: Set a value for a single rbus data model path.
  - `rbusEvent_Subscribe`: Subscribe to rbus events (e.g., value changes, object creation/deletion).
  - `rbusEvent_Unsubscribe`: Unsubscribe from rbus events.
- **Configurable**: Server configuration via a JSON file (`config.json`) or command-line arguments.
- **Graceful Shutdown**: Supports `SIGTERM` for clean resource cleanup.
- **Homebrew Installation**: Installable via a Homebrew formula for macOS/Linux.

## Installation

### Prerequisites

- **Dependencies**:
  - `cmake` and `pkg-config` (build tools)
  - `libwebsockets` (WebSocket library)
  - `jansson` (JSON parsing library)
  - `openssl@3` (SSL support)
  - `rbus` (RDK Bus library, may require a custom tap or manual installation)

On Ubuntu/Debian, install dependencies:
```bash
sudo apt-get install libssl-dev libwebsockets-dev libjansson-dev librbus-dev pkg-config cmake
```

For the JavaScript client:
```bash
npm install ws
```

### Homebrew Installation (macOS/Linux)

1. Create or use a Homebrew tap for `rbus-jsonrpc`:
   ```bash
   brew tap stepherg/tap
   ```
2. Install:
   ```bash
   brew install rbus-jsonrpc
   ```
   This installs `rbus_jsonrpc` to `/opt/homebrew/bin` (or `/usr/local/bin` on Intel macOS) and places `config.json` in `/opt/homebrew/etc/rbus/`.

### Manual Build

1. Clone the repository:
   ```bash
   git clone https://github.com/stepherg/rbus-jsonrpc.git
   cd rbus-jsonrpc
   ```
2. Build:
   ```bash
   mkdir build && cd build
   cmake ..
   make
   ```
3. Install (optional):
   ```bash
   sudo make install
   ```
   This installs binaries to `/usr/local/bin` and `config.json` to `/usr/local/etc/rbus/`.

## Configuration

The server reads configuration from `config.json` (default location: `/opt/homebrew/etc/rbus/config.json` or specified via `-c` option). Example `config.json`:

```json
{
  "host": "localhost",
  "port": 8080,
  "ssl_enabled": false
}
```

- `host`: The server host (e.g., `localhost`, `0.0.0.0`).
- `port`: The server port (1–65535).
- `ssl_enabled`: Set to `true` to enable SSL (requires OpenSSL configuration).

You can override the config file path and values via command-line arguments:
```bash
rbus_jsonrpc -c /path/to/config.json [host] [port]
```

## Usage

### Running the Server

Start the server with the default configuration:
```bash
rbus_jsonrpc
```

Or specify a custom config file and override host/port:
```bash
rbus_jsonrpc -c /etc/rbus/config.json 0.0.0.0 9090
```

The server runs on `ws://<host>:<port>` (e.g., `ws://localhost:8080`) and supports `SIGTERM` for graceful shutdown:
```bash
kill -TERM <pid>
```

### Supported JSON-RPC Methods

The server supports the following JSON-RPC methods:

1. **rbus_get**
   - **Description**: Retrieves values for one or more rbus data model paths.
   - **Parameters**:
     - `path`: A string containing one path or a comma-separated list of paths (e.g., `"Device.DeviceInfo.ModelName,Device.DeviceInfo.SerialNumber"`).
   - **Response**:
     - Returns an object with paths as keys and values (e.g., `{"Device.DeviceInfo.ModelName": "testmodel", "Device.DeviceInfo.SerialNumber": "123456"}`).
   - **Error**: Returns an error object if the path is invalid or not found.

2. **rbus_set**
   - **Description**: Sets a value for a single rbus data model path.
   - **Parameters**:
     - `path`: The rbus path (e.g., `"Device.DeviceInfo.ModelName"`).
     - `value`: The value to set (e.g., string, number, boolean, array, or object).
   - **Response**: Returns `true` on success.
   - **Error**: Returns an error object if the set operation fails.

3. **rbusEvent_Subscribe**
   - **Description**: Subscribes to an rbus event (e.g., value changes, object creation/deletion).
   - **Parameters**:
     - `eventName`: The fully qualified event name (e.g., `"Device.WiFi.SSID.1.Status!"`).
     - `timeout`: Optional retry timeout in seconds (default: 30).
   - **Response**: Returns `true` on success.
   - **Error**: Returns an error object if subscription fails.
   - **Notifications**: Sends JSON-RPC notifications with `method: "rbus_event"`, including `eventName`, `type`, and `data`.

4. **rbusEvent_Unsubscribe**
   - **Description**: Unsubscribes from an rbus event.
   - **Parameters**:
     - `eventName`: The fully qualified event name.
   - **Response**: Returns `true` on success.
   - **Error**: Returns an error object if unsubscription fails.

### JavaScript Client Example

Below is an example JavaScript client using the `ws` library to interact with the server, demonstrating `rbus_get`, `rbus_set`, `rbusEvent_Subscribe`, and `rbusEvent_Unsubscribe`.

```javascript
const WebSocket = require('ws');

const ws = new WebSocket('ws://localhost:8080');

ws.on('open', () => {
   console.log('Connected to WebSocket server');

   ws.send(JSON.stringify({jsonrpc: '2.0', method: 'rbus_get', params: {path: 'Device.DeviceInfo.ModelName,Device.DeviceInfo.SerialNumber'}, id: 1}));
   ws.send(JSON.stringify({jsonrpc: '2.0', method: 'rbus_set', params: {path: 'Device.Test.Property', value: 'test'}, id: 2}));
   ws.send(JSON.stringify({jsonrpc: '2.0', method: 'rbusEvent_Subscribe', params: {eventName: 'Device.Test.Property', timeout: 0}, id: 3}));

   setTimeout(() => {
      // Set Device.Test.Property value to trigger event
      ws.send(JSON.stringify({jsonrpc: '2.0', method: 'rbus_set', params: {path: 'Device.Test.Property', value: 'newtest'}, id: 5}));
   }, 3000);

   setTimeout(() => {
      ws.send(JSON.stringify({jsonrpc: '2.0', method: 'rbusEvent_Unsubscribe', params: {eventName: 'Device.Test.Property'}, id: 4}));
      ws.close();
   }, 10000);
});

ws.on('message', (data) => {
   try {
      const response = JSON.parse(data);
      console.log('Received response:');
      console.log(JSON.stringify(response, null, 2));

      // Handle notifications (no id)
      if (!response.id && response.method === 'rbus_event') {
         console.log(`Event received: ${response.params.eventName} (${response.params.type}): ${JSON.stringify(response.params.data)}`);
         return;
      }

      if (response.error) {
         console.error(`failed: ${response.error.message}`);
      }

   } catch (error) {
      console.error('Error parsing response:', error.message);
      ws.close();
   }
});

ws.on('error', (error) => {
   console.error('WebSocket error:', error.message);
});

ws.on('close', () => {
   console.log('WebSocket connection closed');
});
```

### Running the JavaScript Client

1. Install the `ws` library:
   ```bash
   npm install ws
   ```
2. Save the above code as `client.js`.
3. Ensure the server is running:
   ```bash
   rbus_jsonrpc
   ```
4. Run the client:
   ```bash
   node client.js
   ```

### Example Output

```bash
Connected to WebSocket server
Received response:
{
  "jsonrpc": "2.0",
  "result": {
    "Device.DeviceInfo.ModelName": "testmodel",
    "Device.DeviceInfo.SerialNumber": "G3Q5W9TG09"
  },
  "id": 1
}
Received response:
{
  "jsonrpc": "2.0",
  "result": true,
  "id": 2
}
Received response:
{
  "jsonrpc": "2.0",
  "result": true,
  "id": 3
}
Received response:
{
  "jsonrpc": "2.0",
  "result": true,
  "id": 5
}
Received response:
{
  "jsonrpc": "2.0",
  "method": "rbus_event",
  "params": {
    "eventName": "Device.Test.Property",
    "type": "value_changed",
    "data": "newtest"
  }
}
Event received: Device.Test.Property (value_changed): "newtest"
Received response:
{
  "jsonrpc": "2.0",
  "result": true,
  "id": 4
}
WebSocket connection closed
```

## Notes

- **rbus Dependency**: The `rbus` library may require manual installation or a custom Homebrew tap. Contact the repository maintainers for guidance.
- **SSL Support**: Enable SSL in `config.json` (`"ssl_enabled": true`) for secure connections, but ensure OpenSSL certificates are configured.
- **Event Testing**: Replace `Device.WiFi.SSID.1.Status!` with an actual event name supported by your rbus provider.
- **Contributing**: Contributions are welcome! Submit pull requests to `https://github.com/stepherg/rbus-jsonrpc`.

## License

This project is licensed under the Apache License 2.0. See the [LICENSE](LICENSE) file for details.
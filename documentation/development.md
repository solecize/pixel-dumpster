# Development Guide

This guide covers development practices, architecture decisions, and contribution guidelines for the pixel-dumpster project.

## Architecture Overview

The pixel-dumpster system is built around several core components:

### Core Components

1. **Configuration**
   - Manages persisted device settings (NVS + JSON config file)
   - Validates configuration parameters
   - Stores setup wizard results

2. **Display Driver**
   - Abstracts HUB75 matrix output
   - Handles rotation, buffering, and effects

3. **Setup Wizard**
   - First-run configuration flow
   - USB keyboard input handling (ESP-IDF USB Host)
   - Wi-Fi scanning and connection

4. **Artifact Storage**
   - LittleFS-backed asset store
   - Directory structure validation
   - now.json state management

5. **Notification + API Layer**
   - UDP doorbell notifications
   - HTTP API endpoints (esp_http_server)
   - Polling backstop mechanism

### Design Principles

- **Artifacts are truth**: Display state derived from stored files
- **Push is advisory**: Notifications suggest changes, files confirm
- **Controller-agnostic**: Works with any upstream system
- **Frontend-agnostic**: No specific UI requirements
- **Deterministic fallback**: Always shows something meaningful
- **Minimal API surface**: Simple, focused endpoints

## Development Environment

### Prerequisites

- ESP-IDF toolchain (native workflow)
- ESP32 development board
- LED matrix panel (HUB75)
- USB keyboard for testing setup

### Setup

1. Clone the repository
2. Install ESP-IDF and export the environment (`. $IDF_PATH/export.sh`)
3. Configure settings via `idf.py menuconfig`
4. Build and flash with `idf.py build` + `idf.py flash`

### Build Commands

```bash
# Configure project
idf.py menuconfig

# Build the project
idf.py build

# Flash to device
idf.py flash

# Monitor serial output
idf.py monitor

# Clean build
idf.py fullclean
```

## Code Organization

### Directory Structure

```
main/
├── app-main.c               # ESP-IDF application entry point

data/
└── pd/                      # Default artifact structure
    ├── now.json
    ├── default.png
    ├── system/
    ├── game/
    └── assets/

examples/
├── python-client.py        # Python client example
├── node-client.js          # Node.js client example
└── readme.md               # Client documentation

documentation/
├── pixel-dumpster.md       # Living project document
├── development.md          # This file
└── api.md                  # API reference
```

### Coding Standards

- **C/C++ (ESP-IDF)** compatibility
- **ESP-IDF** conventions (FreeRTOS tasks, event loops, `esp_err_t` handling)
- **kebab-style-naming** for files and folders
- **snake_case** for function and variable names
- **UPPER_CASE** for constants
- **Header guards** for all headers
- **Documentation** for all public APIs

### Memory Management

- **Avoid dynamic allocation** in time-critical code
- **Use stack allocation** where possible
- **Free resources** explicitly in destructors
- **Monitor heap usage** during development

## Testing

### Unit Tests

Create unit tests for core components:

```cpp
// test/test_config_manager.cpp
#include <unity.h>
#include "config_manager.h"

void test_config_validation() {
    ConfigManager manager;
    
    TEST_ASSERT_TRUE(manager.validate_matrix_size(224, 64));
    TEST_ASSERT_FALSE(manager.validate_matrix_size(7, 7));
    TEST_ASSERT_TRUE(manager.validate_orientation(90));
    TEST_ASSERT_FALSE(manager.validate_orientation(45));
}

void setUp() {
    // Test setup
}

void tearDown() {
    // Test cleanup
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_config_validation);
    return UNITY_END();
}
```

### Integration Tests

Test component interactions:

```cpp
// test/test_integration.cpp
#include <unity.h>
#include "artifact_storage.h"
#include "notification_system.h"

void test_artifact_notification_flow() {
    ArtifactStorage storage;
    NotificationSystem notifications(nullptr, &storage);
    
    // Test artifact upload triggers notification
    uint8_t test_data[] = {0x89, 0x50, 0x4E, 0x47}; // PNG header
    TEST_ASSERT_TRUE(storage.save_artifact("/pd/test.png", test_data, 4, CUSTOM));
    
    // Verify notification system responds
    // ... test implementation
}
```

### Hardware Testing

Test with actual hardware:

```cpp
// test/test_hardware.cpp
#include <unity.h>
#include "led_matrix_driver.h"

void test_led_matrix_operations() {
    LEDMatrixDriver matrix;
    TEST_ASSERT_TRUE(matrix.begin(HUB75, 224, 64));
    
    // Test basic operations
    matrix.clear_screen();
    matrix.set_pixel(10, 10, matrix.color565(255, 0, 0));
    TEST_ASSERT_EQUAL(matrix.color565(255, 0, 0), matrix.get_pixel(10, 10));
    
    matrix.end();
}
```

## Debugging

### Serial Debugging

Use ESP-IDF logging:

```c
#include "esp_log.h"

static const char *TAG = "pixel-dumpster";

ESP_LOGI(TAG, "Info message");
ESP_LOGW(TAG, "Warning message");
ESP_LOGE(TAG, "Error message");
```

### Performance Monitoring

Monitor performance and memory:

```cpp
void print_heap_info() {
    ESP_LOGI(TAG, "Free heap: %u bytes", (unsigned int)esp_get_free_heap_size());
}

void measure_function_time(void (*fn)(void)) {
    int64_t start = esp_timer_get_time();
    fn();
    int64_t duration = esp_timer_get_time() - start;
    ESP_LOGI(TAG, "Function took %lld us", duration);
}
```

### Network Debugging

Debug network operations:

```cpp
void debug_wifi_status() {
    ESP_LOGI(TAG, "WiFi status: %s", esp_netif_is_netif_up(netif) ? "up" : "down");
}
```

## Contributing

### Pull Request Process

1. **Fork** the repository
2. **Create** a feature branch (`git checkout -b feature/amazing-feature`)
3. **Make** changes with appropriate tests
4. **Ensure** all tests pass
5. **Update** documentation as needed
6. **Commit** changes with descriptive messages
7. **Push** to your fork
8. **Create** a pull request

### Commit Message Format

Use conventional commit messages:

```
type(scope): description

[optional body]

[optional footer]
```

Types:
- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation
- `style`: Code style
- `refactor`: Refactoring
- `test`: Tests
- `chore`: Maintenance

Examples:
```
feat(matrix): add support for 128x64 panels
fix(wizard): handle WiFi timeout gracefully
docs(api): update endpoint documentation
```

### Code Review Guidelines

- **Functionality**: Does the code work as intended?
- **Style**: Does it follow project conventions?
- **Tests**: Are there adequate tests?
- **Documentation**: Is the code well-documented?
- **Performance**: Are there performance implications?
- **Security**: Are there security considerations?

## Performance Optimization

### Memory Optimization

- **Use appropriate data types** (uint8_t vs int)
- **Avoid string concatenation** in loops
- **Reuse buffers** instead of allocating
- **Monitor heap fragmentation**

### CPU Optimization

- **Minimize blocking operations**
- **Use state machines** instead of delays
- **Optimize drawing operations**
- **Cache frequently accessed data**

### Network Optimization

- **Batch operations** where possible
- **Use appropriate timeouts**
- **Handle connection failures gracefully**
- **Implement retry logic**

## Security Considerations

### Network Security

- **Validate input data** from all sources
- **Use appropriate authentication** for sensitive operations
- **Implement rate limiting** for API endpoints
- **Secure file uploads** with validation

### File System Security

- **Validate file paths** to prevent directory traversal
- **Limit file sizes** to prevent storage exhaustion
- **Validate file types** for uploads
- **Implement proper permissions**

## API Design

### RESTful Principles

- **Use HTTP verbs** correctly (GET, POST, PUT, DELETE)
- **Use resource-based URLs**
- **Return appropriate status codes**
- **Provide consistent error responses**

### Versioning

- **API versioning** in URL path (`/v1/`)
- **Backward compatibility** where possible
- **Deprecation notices** for breaking changes
- **Migration guides** for major versions

### Documentation

- **OpenAPI/Swagger** specification
- **Example requests/responses**
- **Error code documentation**
- **Authentication requirements**

## Deployment

### Build Process

- **Automated builds** for multiple targets
- **Artifact signing** for security
- **Version tagging** for releases
- **Release notes** for each version

### OTA Updates

- **Secure update mechanism**
- **Rollback capability**
- **Update validation**
- **User notification**

### Monitoring

- **Health check endpoints**
- **Performance metrics**
- **Error tracking**
- **Usage analytics**

## Troubleshooting

### Common Issues

1. **Compilation Errors**
   - Check library dependencies
   - Verify platform compatibility
   - Review syntax errors

2. **Upload Failures**
   - Check USB connection
   - Verify driver installation
   - Try different upload speed

3. **Runtime Crashes**
   - Check stack overflow
   - Verify memory allocation
   - Review pointer usage

4. **Network Issues**
   - Check WiFi credentials
   - Verify network availability
   - Review firewall settings

### Debug Tools

- **Serial monitor** for real-time debugging
- **Logic analyzer** for hardware debugging
- **Network sniffer** for protocol analysis
- **Memory profiler** for optimization

## Future Development

### Planned Features

- **PNG decoder** for better image support
- **Animation system** for dynamic content
- **Multiple display support**
- **Cloud integration** for remote management
- **Mobile app** for easy configuration

### Technical Debt

- **Refactor monolithic functions**
- **Improve error handling**
- **Add comprehensive tests**
- **Update documentation**
- **Optimize memory usage**

### Research Areas

- **Low-power operation**
- **Alternative display technologies**
- **Advanced networking protocols**
- **Machine learning integration**
- **Security enhancements**

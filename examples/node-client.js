#!/usr/bin/env node

/**
 * pixel-dumpster Node.js Client Example
 * 
 * This script demonstrates how to interact with a pixel-dumpster device
 * using UDP notifications and HTTP API calls.
 */

const dgram = require('dgram');
const fs = require('fs');
const path = require('path');
const FormData = require('form-data');
const axios = require('axios');

class PixelDumpsterClient {
    constructor(host = 'pixel-dumpster.local', udpPort = 9876, httpPort = 8088) {
        this.host = host;
        this.udpPort = udpPort;
        this.httpPort = httpPort;
        this.baseUrl = `http://${host}:${httpPort}`;
        this.udpClient = dgram.createSocket('udp4');
    }
    
    sendUdpNotification(message = 'refresh') {
        return new Promise((resolve, reject) => {
            const messageBuffer = Buffer.from(message);
            
            this.udpClient.send(messageBuffer, this.udpPort, this.host, (err) => {
                if (err) {
                    console.error(`UDP notification failed: ${err.message}`);
                    reject(err);
                } else {
                    console.log(`UDP notification sent: ${message}`);
                    resolve(true);
                }
            });
        });
    }
    
    async getState() {
        try {
            const response = await axios.get(`${this.baseUrl}/state`);
            return response.data;
        } catch (error) {
            console.error(`Failed to get state: ${error.message}`);
            return null;
        }
    }
    
    async reloadState() {
        try {
            const response = await axios.post(`${this.baseUrl}/reload`);
            if (response.status === 204) {
                console.log('Reload triggered successfully');
                return true;
            } else {
                console.log(`Failed to trigger reload: ${response.status}`);
                return false;
            }
        } catch (error) {
            console.error(`Reload request failed: ${error.message}`);
            return false;
        }
    }
    
    async uploadAsset(filePath, assetName = null) {
        try {
            if (!fs.existsSync(filePath)) {
                console.error(`File not found: ${filePath}`);
                return false;
            }
            
            const filename = assetName || path.basename(filePath);
            const fileStream = fs.createReadStream(filePath);
            
            const form = new FormData();
            form.append('file', fileStream, filename);
            
            const response = await axios.post(`${this.baseUrl}/upload`, form, {
                headers: {
                    ...form.getHeaders()
                }
            });
            
            if (response.status === 200) {
                console.log(`Asset uploaded successfully: ${filename}`);
                return true;
            } else {
                console.log(`Failed to upload asset: ${response.status}`);
                return false;
            }
        } catch (error) {
            console.error(`Upload failed: ${error.message}`);
            return false;
        }
    }
    
    async listAssets() {
        try {
            const response = await axios.get(`${this.baseUrl}/list`);
            return response.data;
        } catch (error) {
            console.error(`Failed to list assets: ${error.message}`);
            return null;
        }
    }
    
    async getStatus() {
        try {
            const response = await axios.get(`${this.baseUrl}/status`);
            return response.data;
        } catch (error) {
            console.error(`Failed to get status: ${error.message}`);
            return null;
        }
    }
    
    async setDisplayMode(mode, system = '', game = '', asset = '') {
        const stateData = {
            mode,
            system,
            game,
            asset,
            updated_at: Math.floor(Date.now() / 1000)
        };
        
        console.log(`Display mode set to: ${mode}`);
        await this.sendUdpNotification();
        return true;
    }
    
    close() {
        this.udpClient.close();
    }
}

// CLI interface
async function main() {
    const args = process.argv.slice(2);
    
    if (args.length === 0) {
        console.log('Usage: node node-client.js <command> [args...]');
        console.log('\nCommands:');
        console.log('  state                    - Get current display state');
        console.log('  reload                   - Trigger reload');
        console.log('  upload <file> [name]      - Upload PNG asset');
        console.log('  list                     - List assets');
        console.log('  status                   - Get system status');
        console.log('  notify [message]         - Send UDP notification');
        console.log('  mode <mode> [system] [game] [asset] - Set display mode');
        console.log('\nExamples:');
        console.log('  node node-client.js state');
        console.log('  node node-client.js upload logo.png');
        console.log('  node node-client.js mode game mame pacman');
        console.log('  node node-client.js notify refresh');
        return;
    }
    
    const client = new PixelDumpsterClient();
    const command = args[0].toLowerCase();
    
    try {
        switch (command) {
            case 'state':
                const state = await client.getState();
                if (state) {
                    console.log(JSON.stringify(state, null, 2));
                }
                break;
                
            case 'reload':
                await client.reloadState();
                break;
                
            case 'upload':
                if (args.length < 3) {
                    console.log('Usage: node node-client.js upload <file> [name]');
                    break;
                }
                await client.uploadAsset(args[1], args[2]);
                break;
                
            case 'list':
                const assets = await client.listAssets();
                if (assets) {
                    console.log(JSON.stringify(assets, null, 2));
                }
                break;
                
            case 'status':
                const status = await client.getStatus();
                if (status) {
                    console.log(JSON.stringify(status, null, 2));
                }
                break;
                
            case 'notify':
                const message = args[1] || 'refresh';
                await client.sendUdpNotification(message);
                break;
                
            case 'mode':
                if (args.length < 3) {
                    console.log('Usage: node node-client.js mode <mode> [system] [game] [asset]');
                    break;
                }
                await client.setDisplayMode(args[1], args[2] || '', args[3] || '', args[4] || '');
                break;
                
            default:
                console.log(`Unknown command: ${command}`);
        }
    } catch (error) {
        console.error(`Error: ${error.message}`);
    } finally {
        client.close();
    }
}

if (require.main === module) {
    main().catch(console.error);
}

module.exports = PixelDumpsterClient;

import { h, Component } from 'preact';

import { Terminal } from './terminal';

import type { ITerminalOptions, ITheme } from '@xterm/xterm';
import type { ClientOptions, FlowControl } from './terminal/xterm';

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const path = window.location.pathname.replace(/[/]+$/, '');
let host = window.location.host;
const fwdPort = new URL(window.location.href).searchParams.get('fwdPort');
if (fwdPort) {
    host = host.replace(/:.*/, ':' + fwdPort);
}
const wsUrl = [protocol, '//', host, path, '/ws', window.location.search].join('');
const tokenUrl = [window.location.protocol, '//', window.location.host, path, '/token'].join('');
const clientOptions = {
    rendererType: 'webgl',
    disableLeaveAlert: false,
    disableResizeOverlay: false,
    enableZmodem: false,
    enableTrzsz: false,
    enableSixel: false,
    closeOnDisconnect: false,
    isWindows: false,
    unicodeVersion: '11',
} as ClientOptions;
const termOptions = {
    fontSize: 14,
    lineHeight: 1,
    fontFamily: 'Google Sans Code,Liberation Mono,Menlo,Courier,monospace',
    theme: {
        // Google Dark theme
        selectionBackground: '#3252b8',
        foreground: '#b0afaf',
        background: '#1f1f1f',
        cursor: '#98b1ff',
        brightWhite: '#fdfcfc',
        white: '#b0afaf',
        brightBlack: '#555555', // change from current google dark
        black: '#333333', // change from current google dark
        blue: '#7895ff',
        brightBlue: '#98b1ff',
        green: '#17b877',
        brightGreen: '#66ce98',
        cyan: '#25a6e9',
        brightCyan: '#71c2ee',
        red: '#f76769',
        brightRed: '#fc8f8e',
        magenta: '#a87ffb',
        brightMagenta: '#c8aaff',
        yellow: '#ffa23e',
        brightYellow: '#ffc26e',
    } as ITheme,
    allowProposedApi: true,
} as ITerminalOptions;
const flowControl = {
    limit: 100000,
    highWater: 10,
    lowWater: 4,
} as FlowControl;

export class App extends Component {
    render() {
        return (
            <Terminal
                id="terminal-container"
                wsUrl={wsUrl}
                tokenUrl={tokenUrl}
                clientOptions={clientOptions}
                termOptions={termOptions}
                flowControl={flowControl}
            />
        );
    }
}

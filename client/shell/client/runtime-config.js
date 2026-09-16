// Bundled deployments omit boundary stacks from audit payloads. Development
// packages can explicitly set this to false; never infer the mode from a URL
// query, localStorage, or an untrusted server response.
Object.defineProperty(window, '__PLINTH_PRODUCTION__', {
    value: true,
    writable: false,
    configurable: false,
});

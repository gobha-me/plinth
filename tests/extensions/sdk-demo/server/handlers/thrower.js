// Test-only real native dispatch exception: no I/O, state, or caller data.
export default function thrower() {
    throw new TypeError('sdkdemo controlled handler failure');
}

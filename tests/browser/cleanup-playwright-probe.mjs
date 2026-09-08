import { rename, writeFile } from 'node:fs/promises';
import { chromium } from 'playwright';

// Disable Playwright's own signal cleanup to exercise the external supervisor,
// including its fallback after Node dies before cleaning the detached browser.
const browser = await chromium.launchServer({
    executablePath: process.env.PLINTH_BROWSER || undefined,
    args: process.env.PLINTH_BROWSER_NO_SANDBOX === '1' ? ['--no-sandbox'] : [],
    handleSIGINT: false,
    handleSIGTERM: false,
    handleSIGHUP: false,
});
await writeFile(process.argv[2] + '.tmp', JSON.stringify({ browser: browser.process().pid }));
await rename(process.argv[2] + '.tmp', process.argv[2]);
await new Promise(() => {});

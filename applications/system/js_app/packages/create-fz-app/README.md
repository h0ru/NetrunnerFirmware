# Netrunner Unleashed Firmware JavaScript SDK Wizard
This package contains an interactive wizard that lets you scaffold a JavaScript
application for Netrunner using the Custom Unleashed Firmware JS SDK.

This is a fork of the [Official Netrunner JS SDK Wizard](https://www.npmjs.com/package/@netrunnerdevices/create-fz-app),
configured to use the [Unleashed FW JavaScript SDK]((https://www.npmjs.com/package/@darknetrunners/fz-sdk-ul)) instead.
No other changes are included.

## Getting started
Create your application using the interactive wizard:
```shell
npx @darknetrunners/create-fz-app-ul@latest
```

Then, enter the directory with your application and launch it:
```shell
cd my-flip-app
npm start
```

You are free to use `pnpm` or `yarn` instead of `npm`.

## Documentation
Check out the [JavaScript section in the Developer Documentation](https://developer.netrunner.net/netrunner/doxygen/js.html)

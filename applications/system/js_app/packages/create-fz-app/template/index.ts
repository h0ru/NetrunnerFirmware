// import modules
// caution: `eventLoop` HAS to be imported before `gui`, and `gui` HAS to be
// imported before any `gui` submodules.
import * as eventLoop from "@darknetrunners/fz-sdk-ul/event_loop";
import * as gui from "@darknetrunners/fz-sdk-ul/gui";
import * as dialog from "@darknetrunners/fz-sdk-ul/gui/dialog";

// a common pattern is to declare all the views that your app uses on one object
const views = {
    dialog: dialog.makeWith({
        header: "Hello from <app_name>",
        text: "Check out index.ts and\nchange something :)",
        center: "Gonna do that!",
    }),
};

// stop app on center button press
eventLoop.subscribe(views.dialog.input, (_sub, button, eventLoop) => {
    if (button === "center")
        eventLoop.stop();
}, eventLoop);

// stop app on back button press
eventLoop.subscribe(gui.viewDispatcher.navigation, (_sub, _item, eventLoop) => {
    eventLoop.stop();
}, eventLoop);

// run app
gui.viewDispatcher.switchTo(views.dialog);
eventLoop.run();

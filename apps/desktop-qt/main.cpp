#include "MainWindow.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QIcon>
#include <QTimer>

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("CodeTrace");
    QApplication::setOrganizationName("CodeTrace");
    QApplication::setApplicationVersion("0.1.0");
    QApplication::setWindowIcon(QIcon(":/codetrace.png"));

    QCommandLineParser parser;
    parser.setApplicationDescription("CodeTrace — static analysis and call graph visualization");
    parser.addHelpOption();
    parser.addVersionOption();
    parser.addPositionalArgument("path", "Project folder or source file to open");
    QCommandLineOption select("select", "Select the function NAME after analysis.", "name");
    QCommandLineOption screenshot("screenshot", "Render the window to FILE after analysis and exit (smoke test).", "file");
    parser.addOption(select);
    parser.addOption(screenshot);
    parser.process(app);

    MainWindow w;
    w.resize(1500, 900);
    w.show();

    QObject::connect(&w, &MainWindow::analysisCompleted, &w, [&](bool ok) {
        if (parser.isSet(select)) w.selectSymbolByName(parser.value(select));
        if (parser.isSet(screenshot)) {
            QTimer::singleShot(300, &w, [&, ok] {
                bool saved = w.grab().save(parser.value(screenshot));
                QApplication::exit(ok && saved ? 0 : 1);
            });
        }
    });
    if (!parser.positionalArguments().isEmpty()) w.openProject(parser.positionalArguments().first());
    else if (parser.isSet(screenshot)) return 2;
    return app.exec();
}

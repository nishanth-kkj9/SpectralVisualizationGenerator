// Phase 18 — SpectraScope: Qt Widgets thin client over SpectraCore.

#include "main_window.h"

#include <QApplication>

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("SpectraScope"));
    app.setOrganizationName(QStringLiteral("SpectralVisualizationGenerator"));
    MainWindow w;
    w.show();
    return app.exec();
}

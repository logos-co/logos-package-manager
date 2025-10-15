#include <QCoreApplication>
#include <QCommandLineParser>
#include <QPluginLoader>
#include <QDebug>
#include <QObject>
#include <QtPlugin>
#include <QString>

// Minimal local declaration of the PluginInterface with the same IID
class PluginInterface
{
public:
    virtual ~PluginInterface() {}
    virtual QString name() const = 0;
    virtual QString version() const = 0;
};

#define PluginInterface_iid "com.example.PluginInterface"
Q_DECLARE_INTERFACE(PluginInterface, PluginInterface_iid)

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("plugin_loader");
    app.setApplicationVersion("1.0");

    QCommandLineParser parser;
    parser.setApplicationDescription("Minimal Qt plugin loader example (Package Manager)");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption pluginPathOption(QStringList() << "p" << "path",
                                       "Path to the plugin file (.so/.dylib)",
                                       "plugin_path");
    parser.addOption(pluginPathOption);

    parser.process(app);

    QString pluginPath = parser.value(pluginPathOption);
    if (pluginPath.isEmpty()) {
        qCritical() << "Plugin path must be specified with --path";
        return 1;
    }

    qInfo() << "Loading plugin from" << pluginPath;

    QPluginLoader loader(pluginPath);
    QObject *plugin = loader.instance();

    if (!plugin) {
        qCritical() << "Failed to load plugin:" << loader.errorString();
        return 1;
    }

    qInfo() << "Plugin loaded successfully.";

    if (PluginInterface *iface = qobject_cast<PluginInterface *>(plugin)) {
        qInfo() << "Plugin name:" << iface->name();
        qInfo() << "Plugin version:" << iface->version();
    } else {
        const QMetaObject *meta = plugin->metaObject();
        if (meta) {
            qInfo() << "Qt class name:" << meta->className();
        }
    }

    return 0;
}



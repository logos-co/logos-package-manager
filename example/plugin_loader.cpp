#include <QCoreApplication>
#include <QCommandLineParser>
#include <QPluginLoader>
#include <QDebug>
#include <QObject>
#include <QtPlugin>
#include <QString>
#include <QMetaObject>
#include <QMetaMethod>
#include <QMetaProperty>
#include <QMetaEnum>

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

    // List all available methods in the plugin
    const QMetaObject *meta = plugin->metaObject();
    if (meta) {
        qInfo() << "\n=== Available Methods ===";
        
        // List all methods (including inherited ones)
        for (int i = 0; i < meta->methodCount(); ++i) {
            QMetaMethod method = meta->method(i);
            QString methodType;
            
            switch (method.methodType()) {
                case QMetaMethod::Method:
                    methodType = "Method";
                    break;
                case QMetaMethod::Signal:
                    methodType = "Signal";
                    break;
                case QMetaMethod::Slot:
                    methodType = "Slot";
                    break;
                case QMetaMethod::Constructor:
                    methodType = "Constructor";
                    break;
                default:
                    methodType = "Unknown";
                    break;
            }
            
            QString accessLevel;
            if (method.access() == QMetaMethod::Public) {
                accessLevel = "Public";
            } else if (method.access() == QMetaMethod::Protected) {
                accessLevel = "Protected";
            } else if (method.access() == QMetaMethod::Private) {
                accessLevel = "Private";
            } else {
                accessLevel = "Unknown";
            }
            
            qInfo() << QString("  %1 %2 %3(%4)")
                       .arg(accessLevel)
                       .arg(methodType)
                       .arg(method.name().constData())
                       .arg(method.parameterNames().join(", "));
        }
        
        qInfo() << "\n=== Properties ===";
        for (int i = 0; i < meta->propertyCount(); ++i) {
            QMetaProperty prop = meta->property(i);
            qInfo() << QString("  Property: %1 (%2)")
                       .arg(prop.name())
                       .arg(prop.typeName());
        }
        
        qInfo() << "\n=== Enums ===";
        for (int i = 0; i < meta->enumeratorCount(); ++i) {
            QMetaEnum enumerator = meta->enumerator(i);
            qInfo() << QString("  Enum: %1").arg(enumerator.name());
            for (int j = 0; j < enumerator.keyCount(); ++j) {
                qInfo() << QString("    %1 = %2")
                           .arg(enumerator.key(j))
                           .arg(enumerator.value(j));
            }
        }
    }

    return 0;
}



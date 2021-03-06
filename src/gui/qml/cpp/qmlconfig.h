#ifndef QMLCONFIG_H
#define QMLCONFIG_H
#include <QtCore>
#include <boost/shared_ptr.hpp>

class ConfigFile;

class QmlConfig: public QObject
{
    Q_OBJECT
public:

    Q_INVOKABLE QString readConfigString(QString);
    Q_INVOKABLE QStringList readConfigStringList(QString);
    Q_INVOKABLE QString readConfigIntString(QString);
    Q_INVOKABLE QStringList readConfigIntStringList(QString);

    QmlConfig(boost::shared_ptr<ConfigFile>);
    ~QmlConfig();
    QmlConfig(const QmlConfig&);

private:
    boost::shared_ptr<ConfigFile> myConfig;
};

#endif // QMLCONFIG_H

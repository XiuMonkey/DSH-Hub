#pragma once

// ------------------------------------------------------------------
// DllJsonCaller.h
// ------------------------------------------------------------------
// 读取 JSON5 格式的 DLL 描述文件，按照描述调用 DLL 导出函数。
//
// 当前支持的调用约定（Calling Convention）：
//   {
//     "Style": "json",
//     "ReturnType": "int" | "void" | "string",
//     "ResultIsJson": true
//   }
//
// Style = "json" 时，DLL 函数使用统一桥接签名：
//   int    Func(const char* argsJson, char** resultJson);
//   void   Func(const char* argsJson, char** resultJson);
//   string Func(const char* argsJson); // 返回 const char*，内容是 JSON
//
// 工具参数 args 会先被序列化成 JSON 字符串，再传给 DLL。
// ------------------------------------------------------------------

#include <QJsonObject>
#include <QJsonValue>
#include <QLibrary>
#include <QString>
#include <QStringList>
#include <QVector>

class DllJsonCaller
{
public:
    struct ParameterSpec
    {
        QString name;
        QString type;
        QString from;
        QJsonValue defaultValue;
    };

    struct FunctionSpec
    {
        QString function;   // DLL 导出函数名
        QString tool;       // DSH 工具名
        QString style;      // "json"（当前支持）
        QString returnType; // "int" / "void" / "string"
        bool resultIsJson = true;
        QVector<ParameterSpec> parameters;
    };

    struct Descriptor
    {
        QString name;
        QString description;
        QVector<FunctionSpec> functions;
    };

    DllJsonCaller();
    ~DllJsonCaller();

    // 读取并解析 JSON5 描述文件
    bool loadDescriptor(const QString &json5Path);

    // 加载 DLL 文件
    bool loadLibrary(const QString &dllPath);

    bool isReady() const;
    QStringList tools() const;
    QString errorString() const;

    // 按 tool 名调用 DLL；args 会转成 JSON 字符串传给 DLL
    bool callTool(const QString &tool,
                  const QJsonObject &args,
                  QJsonObject &result,
                  QString *errorMessage = nullptr);

private:
    bool parseDescriptor(const QByteArray &json5, Descriptor *out, QString *error);
    bool invokeJsonFunction(const FunctionSpec &fn,
                            const QJsonObject &args,
                            QJsonObject &result,
                            QString *error);
    bool invokeNativeFunction(const FunctionSpec &fn,
                              const QJsonObject &args,
                              QJsonObject &result,
                              QString *error);

    QByteArray stripJson5Comments(const QByteArray &input);
    QByteArray removeTrailingCommas(const QByteArray &input);

    Descriptor m_descriptor;
    QLibrary *m_library = nullptr;
    QString m_errorString;
    QString m_dllPath;
};

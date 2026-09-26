#pragma once

#include <QObject>

class TestThunk : public QObject
{
	Q_OBJECT

private slots:
	void testNoArg();
	void testOneInt();
	void testAdd3();
	void testMix();
	void testDoubleOnly();
	void testDoubleToInt();
	void testDoubleReturnNoArg();
	void testEcho();
	void testPointer32();
	void testPointer64();
	void testVoid();
	void testStackArguments();
	void testMaximumStackArguments();
	void testTooManyArguments();
	void testSignatureFromJson();
	void testNativeExtensionViaDllCaller();
	void testMultiExtensionDllCaller();
	void testConcurrentDllCaller();
};

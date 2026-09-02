#include "Fixtures.h"

#include "core/Checksum.h"

#include <QTest>

using namespace immichksync;

/// The checksum is the only identifier a local file and a remote asset share, so the
/// trap these tests exist for is a hex string being compared against a base64 one:
/// every asset would silently look new, and a full re-download would follow.
class ChecksumTest : public QObject {
    Q_OBJECT

private Q_SLOTS:
    void hexRoundTrips()
    {
        const QString hex = QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709");
        const auto checksum = Sha1Checksum::fromHex(hex);
        QVERIFY(checksum.has_value());
        QCOMPARE(checksum->hex(), hex);
        QCOMPARE(checksum->bytes().size(), Sha1Checksum::kByteCount);
    }

    void base64RoundTrips()
    {
        const auto fromHex = Sha1Checksum::fromHex(
            QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd80709"));
        QVERIFY(fromHex.has_value());
        const auto fromBase64 = Sha1Checksum::fromBase64(fromHex->base64());
        QVERIFY(fromBase64.has_value());
        QCOMPARE(*fromBase64, *fromHex);
    }

    void base64IsTheCanonicalDescription()
    {
        const auto checksum = Fixture::checksum(1);
        QCOMPARE(checksum.base64().size(), Sha1Checksum::kBase64Length);
        QCOMPARE(checksum.hex().size(), Sha1Checksum::kHexLength);
    }

    void encodedAcceptsEitherForm()
    {
        const auto expected = Fixture::checksum(42);
        QCOMPARE(*Sha1Checksum::fromEncoded(expected.hex()), expected);
        QCOMPARE(*Sha1Checksum::fromEncoded(expected.base64()), expected);
    }

    void rejectsWrongLengths()
    {
        QVERIFY(!Sha1Checksum::fromHex(QStringLiteral("abcdef")).has_value());
        QVERIFY(!Sha1Checksum::fromBase64(QStringLiteral("abcdef")).has_value());
        QVERIFY(!Sha1Checksum::fromEncoded(QString()).has_value());
        // 41 hex characters: one too many, and a lenient parser would take the first 40.
        QVERIFY(!Sha1Checksum::fromHex(QStringLiteral("da39a3ee5e6b4b0d3255bfef95601890afd807090"))
                     .has_value());
    }

    void rejectsNonHexOfTheRightLength()
    {
        // QByteArray::fromHex silently skips non-hex characters, which would turn this
        // into a short — and therefore plausible-looking — digest.
        QVERIFY(!Sha1Checksum::fromHex(QStringLiteral("zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"))
                     .has_value());
    }

    void rejectsNonBase64OfTheRightLength()
    {
        QVERIFY(!Sha1Checksum::fromBase64(QStringLiteral("!!!!!!!!!!!!!!!!!!!!!!!!!!!!")).has_value());
    }

    void rejectsRawBytesOfTheWrongSize()
    {
        QVERIFY(!Sha1Checksum::fromRaw(QByteArray(19, '\0')).has_value());
        QVERIFY(!Sha1Checksum::fromRaw(QByteArray(21, '\0')).has_value());
        QVERIFY(Sha1Checksum::fromRaw(QByteArray(20, '\0')).has_value());
    }

    void distinctSeedsProduceDistinctChecksums()
    {
        QVERIFY(Fixture::checksum(1) != Fixture::checksum(2));
        QCOMPARE(Fixture::checksum(7), Fixture::checksum(7));
    }

    void ordersByBase64SoPlansAreDeterministic()
    {
        const auto a = Fixture::checksum(1);
        const auto b = Fixture::checksum(2);
        QCOMPARE(a < b, a.base64() < b.base64());
    }

    void shortHexIsTheFirstEightHexDigits()
    {
        const auto checksum = Fixture::checksum(255);
        QCOMPARE(checksum.shortHex(), checksum.hex().left(8));
        QCOMPARE(checksum.shortHex().size(), 8);
    }

    void defaultConstructedIsNull() { QVERIFY(Sha1Checksum().isNull()); }
};

QTEST_APPLESS_MAIN(ChecksumTest)
#include "ChecksumTest.moc"

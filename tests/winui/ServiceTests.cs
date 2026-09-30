using System;
using System.IO;
using System.Net;
using System.Net.Http;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using CastMirror.Services;
using Xunit;

namespace CastMirror.Tests
{
    public class UpdateServiceTests
    {
        [Theory]
        [InlineData("v1.2.3", 1, 2, 3)]
        [InlineData("1.2.3", 1, 2, 3)]
        [InlineData("V2.0.0-rc1", 2, 0, 0)]
        [InlineData("v3.4.5+build7", 3, 4, 5)]
        public void ParseTagAcceptsReleaseTags(string tag, int major, int minor, int patch)
        {
            Assert.Equal(new Version(major, minor, patch), UpdateService.ParseTag(tag));
        }

        [Theory]
        [InlineData(null)]
        [InlineData("")]
        [InlineData("latest")]
        [InlineData("nightly-2026")]
        public void ParseTagRejectsNonVersions(string? tag)
        {
            Assert.Null(UpdateService.ParseTag(tag));
        }

        private sealed class FakeHandler : HttpMessageHandler
        {
            private readonly HttpStatusCode _status;
            private readonly string _body;
            public FakeHandler(HttpStatusCode status, string body) { _status = status; _body = body; }

            protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken) =>
                Task.FromResult(new HttpResponseMessage(_status) { Content = new StringContent(_body, Encoding.UTF8, "application/json") });
        }

        [Fact]
        public async Task NewerReleaseIsReported()
        {
            var handler = new FakeHandler(HttpStatusCode.OK,
                "{\"tag_name\":\"v99.0.0\",\"html_url\":\"https://example.test/r\"}");
            UpdateInfo? update = await UpdateService.CheckAsync(handler);
            Assert.NotNull(update);
            Assert.Equal(new Version(99, 0, 0), update!.Latest);
            Assert.Equal("https://example.test/r", update.PageUrl);
        }

        [Fact]
        public async Task SameOrOlderReleaseIsNotAnUpdate()
        {
            var handler = new FakeHandler(HttpStatusCode.OK, "{\"tag_name\":\"v0.0.1\"}");
            Assert.Null(await UpdateService.CheckAsync(handler));
        }

        [Fact]
        public async Task ServerErrorSurfacesInsteadOfClaimingUpToDate()
        {
            var handler = new FakeHandler(HttpStatusCode.Forbidden, "{}");
            await Assert.ThrowsAsync<HttpRequestException>(() => UpdateService.CheckAsync(handler));
        }
    }

    public class DiagnosticsServiceTests
    {
        [Fact]
        public void MaskAddressesHidesTheLastOctetOnly()
        {
            string masked = DiagnosticsService.MaskAddresses("connect 192.168.7.42:8009 via 10.0.0.5");
            Assert.Equal("connect 192.168.7.x:8009 via 10.0.0.x", masked);
        }

        [Fact]
        public void StripAnsiRemovesColourCodes()
        {
            Assert.Equal("[INFO ] hi", DiagnosticsService.StripAnsi("\u001b[32m[INFO ] hi\u001b[0m"));
        }

        [Fact]
        public void TailOfReturnsOnlyTheLastLinesEvenWhenAnotherProcessHoldsTheFile()
        {
            string path = Path.GetTempFileName();
            try
            {
                File.WriteAllLines(path, new[] { "a", "b", "c", "d", "e" });
                using var writer = new FileStream(path, FileMode.Open, FileAccess.ReadWrite, FileShare.ReadWrite);
                var tail = DiagnosticsService.TailOf(path, 2);
                Assert.Equal(new[] { "d", "e" }, tail);
            }
            finally
            {
                File.Delete(path);
            }
        }

        [Fact]
        public void TailOfMissingFileIsEmpty()
        {
            Assert.Empty(DiagnosticsService.TailOf(Path.Combine(Path.GetTempPath(), "no-such-castmirror.log"), 5));
        }

        [Fact]
        public void BuildMasksAddressesAndNamesTheSections()
        {
            string bundle = DiagnosticsService.Build("{\"ip\":\"192.168.1.20\"}");
            Assert.Contains("CastMirror diagnostics", bundle);
            Assert.Contains("192.168.1.x", bundle);
            Assert.DoesNotContain("192.168.1.20", bundle);
            Assert.Contains("Engine log", bundle);
            Assert.Contains("App log", bundle);
        }
    }
}

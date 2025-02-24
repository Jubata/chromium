// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * Fetches all the urls, and calls {@link createGraph} with the JSON contents.
 */
function fetchAllAndCreateGraph(urls) {
  Promise.all(
      urls.map(url => fetch(url)
               .then(response => response.ok ? response.json() : undefined)))
      .then(jsonContents => createGraph.apply(this, jsonContents));
}

const CODE_PAGE = "code_page";
const REACHED_PAGE = "reached_page";

/**
 * @return {string} the fill color for a given page slice.
 */
function getFillColor(d) {
  switch (d.type) {
    case CODE_PAGE:
      return pageFillColor(d.data);
    case REACHED_PAGE:
      return reachedPageFillColor(d.data);
  }
}

// Prefixes, color, description.
const colors = d3.scale.category20();
let colorIndex = 1;
const COLOR_MAPPING = [
  [["third_party/WebKit"], colors(colorIndex++), "Blink"],
  [["v8"], colors(colorIndex++), "V8"],
  [["base"], colors(colorIndex++), "//base"],
  [["content"], colors(colorIndex++), "//content"],
  [["components"], colors(colorIndex++), "//components"],
  [["chrome/android"], colors(colorIndex++), "//chrome/android"],
  [["chrome"], colors(colorIndex++), "//chrome"],
  [["net", "third_party/boringssl"], colors(colorIndex++), "Net"],
  [["third_party/webrtc", "third_party/opus", "third_party/usrsctp"],
   colors(colorIndex++), "WebRTC"],
  [["third_party/ffmpeg", "third_party/libvpx"], colors(colorIndex++), "Media"],
  [["third_party/icu"], colors(colorIndex++), "ICU"],
  [["skia"], colors(colorIndex++), "Skia"],
  [["ui"], colors(colorIndex++), "UI"],
  [["cc"], colors(colorIndex++), "CC"]];

/**
 * @return the fill color for an item representing a code page.
 */
function pageFillColor(page) {
  const sizeAndFilename = page.size_and_filenames[0];
  if (!sizeAndFilename) return "lightgrey";

  const dominantFilename = sizeAndFilename[1];
  for (let i = 0; i < COLOR_MAPPING.length; i++) {
    const prefixes = COLOR_MAPPING[i][0];
    const color = COLOR_MAPPING[i][1];
    for (let j = 0; j < prefixes.length; j++) {
      if (dominantFilename.startsWith(prefixes[j])) return color;
    }
  }
  return "darkgrey";
}

const reachedColorScale = d3.scale.linear()
      .domain([0, 100])
      .range(["black", "green"]);
/**
 * @return the fill color for an item reprensting reached data.
 */
function reachedPageFillColor(page) {
  if (page.total == 0) return "white";
  if (page.reached == 0) return "red";
  const percentage = 100 * page.reached / page.total;
  return reachedColorScale(percentage);
}

/**
 * Adds the color legend to the document.
 */
function buildColorLegend() {
  let table = document.getElementById("colors-legend-table");
  for (let i = 0; i < COLOR_MAPPING.length; i++) {
    let color = COLOR_MAPPING[i][1];
    let name = COLOR_MAPPING[i][2];
    let row = document.createElement("tr");
    row.setAttribute("align", "left");
    let colorRectangle = document.createElement("td");
    colorRectangle.setAttribute("class", "legend-rectangle");
    colorRectangle.setAttribute("style", `background-color: ${color}`);
    let label = document.createElement("td");
    label.innerHTML = name;
    row.appendChild(colorRectangle);
    row.appendChild(label);
    table.appendChild(row);
  }
}

/**
 * Updates the legend.
 *
 * @param offset Offset of the item to label in the legend.
 * @param offsetToData {offset: data} map.
 */
function updateLegend(offset, offsetToData) {
  let data = offsetToData[offset];
  if (!data) return;

  let page = data[CODE_PAGE];
  let reached = data[REACHED_PAGE];

  let legend = document.getElementById("legend");
  legend.style.display = "block";

  let title = document.getElementById("legend-title");
  let sizeAndFilename = page.size_and_filenames[0];
  let filename = "";
  if (sizeAndFilename) filename = sizeAndFilename[1];

  let reachedSize = reached ? reached.reached : 0;
  let reachedPercentage = reachedSize ? 100 * reachedSize / reached.total : 0;
  title.innerHTML = `
    <b>Page offset:</b> ${page.offset}
    <br/>
    <b>Accounted for:</b> ${page.accounted_for}
    <br/>
    <b>Reached: </b> ${reachedSize} (${reachedPercentage.toFixed(2)}%)
    <br/>
    <b>Dominant filename:</b> ${filename}`;

  let table = document.getElementById("legend-table");
  table.innerHTML = "";
  let header = document.createElement("tr");
  header.setAttribute("align", "left");
  header.innerHTML = "<th>Size</th><th>Filename</th>";
  table.appendChild(header);
  for (let i = 0; i < page.size_and_filenames.length; i++) {
    let row = document.createElement("tr");
    row.setAttribute("align", "left");
    row.innerHTML = `<td>${page.size_and_filenames[i][0]}</td>
                     <td>${page.size_and_filenames[i][1]}</td>`;
    table.appendChild(row);
  }
}


/**
 * @return {int} the lane index for a given data item.
 */
function typeToLane(d) {
  switch (d.type) {
    case CODE_PAGE:
      return 0;
    case REACHED_PAGE:
      return 1;
  }
}

/**
 * Returns: offset -> {"code_page": codeData, "reached": reachedData}
 */
function getOffsetToData(flatData) {
  let result = [];
  for (let i = 0; i < flatData.length; i++) {
    let data = flatData[i].data;
    let type = flatData[i].type;
    let offset = data["offset"];
    if (!result[offset]) result[offset] = {};
    result[offset][type] = data;
  }
  return result;
}

/**
 * Creates the graph, and adds it to the DOM.
 *
 * reachedData can be undefined. In this case, only the code page data is shown.
 *
 * @param codePages data relative to code pages and their content.
 * @param reachedData data relative to which fraction of code pages is reached.
 */
function createGraph(codePages, reachedPerPage) {
  const PAGE_SIZE = 4096;

  let offsets = codePages.map((x) => x.offset).sort((a, b) => a - b);
  let minOffset = +offsets[0];
  let maxOffset = +offsets[offsets.length - 1] + PAGE_SIZE;

  let lanes = reachedPerPage ? 2 : 1;

  // [{type: REACHED_PAGE | CODE_PAGE, data: pageData}]
  let flatData = codePages.map((page) => ({"type": CODE_PAGE, "data": page}));
  if (reachedPerPage) {
    let typedReachedPerPage = reachedPerPage.map(
        (page) => ({"type": REACHED_PAGE, "data": page}));
    flatData = flatData.concat(typedReachedPerPage);
  }
  const offsetToData = getOffsetToData(flatData);

  let margins = [20, 15, 15, 120]  // top right bottom left
  let width = window.innerWidth - margins[1] - margins[3]
  let height = 500 - margins[0] - margins[2]
  let miniHeight = lanes * 12 + 50
  let mainHeight = height - miniHeight - 50

  let globalXScale = d3.scale.linear().domain([minOffset, maxOffset])
      .range([0, width]);
  const globalScalePageWidth = globalXScale(PAGE_SIZE) - globalXScale(0);

  let zoomedXScale = d3.scale.linear().domain([minOffset, maxOffset])
      .range([0, width]);

  let mainYScale = d3.scale.linear().domain([0, lanes]).range([0, mainHeight]);
  let miniYScale = d3.scale.linear().domain([0, lanes]).range([0, miniHeight]);

  let drag = d3.behavior.drag().on("drag", dragmove);

  let chart = d3.select("body")
      .append("svg")
      .attr("width", width + margins[1] + margins[3])
      .attr("height", height + margins[0] + margins[2])
      .attr("class", "chart");

  chart.append("defs").append("clipPath")
      .attr("id", "clip")
      .append("rect")
      .attr("width", width)
      .attr("height", mainHeight);

  let main = chart.append("g")
      .attr("transform", `translate(${margins[3]}, ${margins[0]})`)
      .attr("width", width)
      .attr("height", mainHeight)
      .attr("class", "main");

  let mini = chart.append("g")
      .attr("transform", `translate(${margins[3]}, ${mainHeight + margins[0]})`)
      .attr("width", width)
      .attr("height", miniHeight)
      .attr("class", "mini");

  let miniAxisScale = d3.scale.linear()
      .domain([0, (maxOffset - minOffset) / 1e6])
      .range([0, width]);
  let miniAxis = d3.svg.axis().scale(miniAxisScale).orient("bottom");
  let miniXAxis = mini.append("g")
      .attr("transform", `translate(0, ${miniHeight})`)
      .call(miniAxis);

  let mainAxisScale = d3.scale.linear()
      .domain([0, (maxOffset - minOffset) / 1e6])
      .range([0, width]);
  let mainAxis = d3.svg.axis().scale(mainAxisScale).orient("bottom");
  let mainXAxis = main.append("g")
      .attr("class", "x axis")
      .attr("transform", `translate(0, -10)`)
      .call(mainAxis);

  const labels = ["Component", "Reached"].slice(0, lanes);
  main.append("g").selectAll(".laneLines")
      .data(labels)
      .enter().append("line")
      .attr("x1", margins[1])
      .attr("y1", (d, i) => mainYScale(i))
      .attr("x2", width)
      .attr("y2", (d, i) => mainYScale(i))
      .attr("stroke", "lightgray");

  main.append("g").selectAll(".laneText")
      .data(labels)
      .enter().append("text")
      .text((d) => d)
      .attr("x", -margins[1])
      .attr("y", (d, i) => mainYScale(i + .5))
      .attr("dy", ".5ex")
      .attr("text-anchor", "end")
      .attr("class", "laneText");

  let itemRects = main.append("g")
      .attr("clip-path", "url(#clip)");

  mini.append("g").selectAll(".laneLines")
      .data(labels)
      .enter().append("line")
      .attr("x1", margins[1])
      .attr("y1", (d, i) => miniYScale(i))
      .attr("x2", width)
      .attr("y2", (d, i) => miniYScale(i))
      .attr("stroke", "lightgray");

  mini.append("g").selectAll(".laneText")
      .data(labels)
      .enter().append("text")
      .text((d) => d)
      .attr("x", -margins[1])
      .attr("y", (d, i) => miniYScale(i + .5))
      .attr("dy", ".5ex")
      .attr("text-anchor", "end");

  mini.append("g").selectAll("miniItems")
      .data(flatData)
      .enter().append("rect")
      .attr("class", (d) => "miniItem")
      .attr("x", (d) => globalXScale(d.data.offset))
      .attr("y", (d) => miniYScale(typeToLane(d) + .5) - 5)
      .attr("width", (d) => globalScalePageWidth)
      .attr("height", 10)
      .style("fill", getFillColor);

  let brush = d3.svg.brush().x(globalXScale).on("brush", display);
  mini.append("g")
      .attr("class", "x brush")
      .call(brush)
      .selectAll("rect")
      .attr("y", 1)
      .attr("height", miniHeight - 1);

  display();

  function display() {
    let rects, labels,
    minExtent = brush.extent()[0];
    maxExtent = brush.extent()[1];
    visibleItems = flatData.filter((d) => d.data.offset < maxExtent &&
        d.data.offset + PAGE_SIZE > minExtent);

    mini.select(".brush").call(brush.extent([minExtent, maxExtent]));
    zoomedXScale.domain([minExtent, maxExtent]);

    mainAxisScale.domain([(minExtent - minOffset) / 1e6,
                          (maxExtent - minOffset) / 1e6]);
    chart.selectAll(".axis").call(mainAxis);

    // Update the main rects.
    const zoomedXScalePageWidth = zoomedXScale(PAGE_SIZE) - zoomedXScale(0);
    rects = itemRects.selectAll("rect")
        .data(visibleItems, (d) => d.data.offset + d.type)
        .attr("x", (d) => zoomedXScale(d.data.offset))
        .attr("width", (d) => zoomedXScalePageWidth);

    rects.enter().append("rect")
        .attr("class", (d) => "mainItem")
        .attr("x", (d) =>  zoomedXScale(d.data.offset))
        .attr("y", (d) => mainYScale(typeToLane(d)) + 10)
        .attr("width", (d) => zoomedXScalePageWidth)
        .attr("height", (d) => .8 * mainYScale(1))
        .style("fill", getFillColor)
        .on("mouseover", (d) => updateLegend(d.data.offset, offsetToData));

    rects.exit().remove();
  }

  function dragmove(d) {
    d3.select(this).attr("x", d.x = Math.max(0, d3.event.x));
  }
}
